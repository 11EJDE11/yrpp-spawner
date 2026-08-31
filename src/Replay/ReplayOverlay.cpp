/**
*  yrpp-spawner
*
*  Copyright(C) 2026-present CnCNet
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.If not, see <http://www.gnu.org/licenses/>.
*/

#include "ReplayOverlay.h"
#include "ReplayControls.h"
#include "ReplaySeek.h"
#include "ReplaySystem.h"
#include "ReplaySystem.Internal.h"

#include <Drawing.h>
#include <Surface.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>

using namespace ReplaySystem::Internal;

namespace ReplaySystem
{
	namespace Overlay
	{
		namespace
		{
#pragma region Layout

			// One place the panel's shape is decided, so drawing and hit testing cannot drift apart.
			constexpr int PanelHeight = 58;
			constexpr int PanelMaxWidth = 620;
			constexpr int PanelMinWidth = 320;
			// Clear of the bottom screen edge, which is also the edge-scroll strip.
			constexpr int PanelBottomMargin = 14;
			constexpr int PanelPadding = 10;

			constexpr int ButtonWidth = 30;
			constexpr int ButtonHeight = 20;
			constexpr int ButtonGap = 4;
			constexpr int ButtonRowTop = 7;

			constexpr int TrackHeight = 10;
			constexpr int TrackTop = 35;
			constexpr int HandleWidth = 7;
			constexpr int HandleOverhang = 3;

			// How wide a keyframe tick is drawn on the track.
			constexpr int KeyframeTickWidth = 1;
			constexpr int MaxDrawnKeyframes = 256;

			enum ButtonID
			{
				Button_JumpStart = 0,
				Button_StepBack,
				Button_Slower,
				Button_PlayPause,
				Button_Faster,
				Button_StepForward,
				Button_JumpEnd,
				ButtonCount
			};

			struct PanelLayout
			{
				bool Valid = false;
				RectangleStruct Panel {};
				RectangleStruct Buttons[ButtonCount] {};
				RectangleStruct Track {};
				// Where the clock and the speed readout are drawn from; both are right-aligned so
				// the digits changing width does not shuffle the row about.
				Point2D ClockRight {};
				Point2D SpeedRight {};
			};

			bool Contains(const RectangleStruct& rect, int x, int y)
			{
				return x >= rect.X && x < rect.X + rect.Width
					&& y >= rect.Y && y < rect.Y + rect.Height;
			}

			PanelLayout ComputeLayout()
			{
				PanelLayout layout {};

				// Sit inside the tactical view rather than the whole screen, so the sidebar never
				// covers an end of the bar.
				const RectangleStruct& view = DSurface::ViewBounds;
				if (view.Width <= 0 || view.Height <= 0)
					return layout;

				const int width = std::clamp(view.Width - 2 * PanelPadding, PanelMinWidth, PanelMaxWidth);
				if (width > view.Width)
					return layout;

				layout.Panel.Width = width;
				layout.Panel.Height = PanelHeight;
				layout.Panel.X = view.X + (view.Width - width) / 2;
				layout.Panel.Y = view.Y + view.Height - PanelHeight - PanelBottomMargin;

				if (layout.Panel.Y < view.Y)
					return layout;

				int buttonX = layout.Panel.X + PanelPadding;
				const int buttonY = layout.Panel.Y + ButtonRowTop;
				for (int i = 0; i < ButtonCount; ++i)
				{
					layout.Buttons[i] = { buttonX, buttonY, ButtonWidth, ButtonHeight };
					buttonX += ButtonWidth + ButtonGap;
				}

				const int panelRight = layout.Panel.X + layout.Panel.Width - PanelPadding;
				layout.SpeedRight = { panelRight, buttonY + 4 };
				layout.ClockRight = { panelRight - 60, buttonY + 4 };

				layout.Track.X = layout.Panel.X + PanelPadding;
				layout.Track.Y = layout.Panel.Y + TrackTop;
				layout.Track.Width = layout.Panel.Width - 2 * PanelPadding;
				layout.Track.Height = TrackHeight;

				layout.Valid = true;
				return layout;
			}

			// The track's hit area is taller than the track itself: a 10 pixel bar is a hard thing
			// to hit, and the handle sticks out above and below it anyway.
			RectangleStruct TrackHitArea(const PanelLayout& layout)
			{
				return {
					layout.Track.X - HandleWidth,
					layout.Track.Y - HandleOverhang - 3,
					layout.Track.Width + 2 * HandleWidth,
					layout.Track.Height + 2 * (HandleOverhang + 3)
				};
			}

#pragma endregion Layout

#pragma region Palette

			int Rgb(int r, int g, int b)
			{
				return Drawing::RGB_To_Int(r, g, b);
			}

			ColorStruct PanelFill() { return ColorStruct { 8, 10, 14 }; }

			int ColorPanelEdge()     { return Rgb(90, 100, 120); }
			int ColorButtonFill()    { return Rgb(38, 44, 56); }
			int ColorButtonHover()   { return Rgb(60, 72, 92); }
			int ColorButtonPressed() { return Rgb(96, 116, 148); }
			int ColorButtonEdge()    { return Rgb(112, 126, 150); }
			int ColorGlyph()         { return Rgb(224, 230, 240); }
			int ColorGlyphDim()      { return Rgb(112, 120, 134); }
			int ColorTrack()         { return Rgb(30, 35, 44); }
			int ColorTrackEdge()     { return Rgb(84, 94, 112); }
			int ColorElapsed()       { return Rgb(96, 168, 232); }
			int ColorScrub()         { return Rgb(232, 176, 72); }
			int ColorKeyframeTick()  { return Rgb(70, 116, 156); }
			int ColorHandle()        { return Rgb(236, 240, 248); }
			int ColorText()          { return Rgb(216, 222, 232); }
			int ColorTextDim()       { return Rgb(140, 150, 166); }

#pragma endregion Palette

#pragma region Drawing primitives

			void FillRect(const RectangleStruct& rect, int color)
			{
				if (rect.Width <= 0 || rect.Height <= 0)
					return;

				RectangleStruct copy = rect;
				DSurface::Composite->FillRect(&copy, color);
			}

			void DrawRect(const RectangleStruct& rect, int color)
			{
				if (rect.Width <= 0 || rect.Height <= 0)
					return;

				RectangleStruct copy = rect;
				DSurface::Composite->DrawRect(&copy, color);
			}

			void FillRectTranslucent(const RectangleStruct& rect, ColorStruct color, int opacity)
			{
				if (rect.Width <= 0 || rect.Height <= 0)
					return;

				RectangleStruct copy = rect;
				DSurface::Composite->FillRectTrans(&copy, &color, opacity);
			}

			// Glyphs are built from one-pixel rows rather than a font, so the transport symbols look
			// like transport symbols at any screen size instead of like punctuation.
			void DrawTriangle(int x, int y, int width, int height, bool pointsRight, int color)
			{
				const int half = height / 2;
				if (half <= 0)
					return;

				for (int row = 0; row < height; ++row)
				{
					const int fromCentre = std::abs(row - half);
					const int rowWidth = std::max(1, width - (fromCentre * width) / half);
					const int rowX = pointsRight ? x : x + (width - rowWidth);

					FillRect({ rowX, y + row, rowWidth, 1 }, color);
				}
			}

			void DrawPauseBars(int x, int y, int width, int height, int color)
			{
				const int barWidth = std::max(2, width / 3);
				FillRect({ x, y, barWidth, height }, color);
				FillRect({ x + width - barWidth, y, barWidth, height }, color);
			}

			void DrawTextRightAligned(const wchar_t* text, const Point2D& rightEdge, int color)
			{
				RectangleStruct bounds = DSurface::Composite->GetRect();
				Point2D location = rightEdge;

				DSurface::Composite->DrawText(text, &bounds, &location, color, 0,
					TextPrintType::Point6 | TextPrintType::Right | TextPrintType::FullShadow);
			}

#pragma endregion Drawing primitives

#pragma region Interaction state

			struct InteractionState
			{
				bool LeftDownLastFrame = false;
				int PressedButton = -1;
				bool DraggingHandle = false;
				// Where the handle has been dragged to, shown on the bar until the drag is let go
				// and the seek actually runs.
				int ScrubFrame = 0;
				int HoveredButton = -1;
			};

			InteractionState Interaction;

			bool IsLeftButtonDown()
			{
				return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
			}

			int FrameAtTrackPosition(const PanelLayout& layout, int mouseX)
			{
				const int total = std::max(1, Seek::TotalFrames());
				const int width = std::max(1, layout.Track.Width);
				const int offset = std::clamp(mouseX - layout.Track.X, 0, width);

				return static_cast<int>((static_cast<long long>(offset) * total) / width);
			}

			int TrackPositionForFrame(const PanelLayout& layout, int frame)
			{
				const int total = std::max(1, Seek::TotalFrames());
				const int clamped = std::clamp(frame, 0, total);

				return layout.Track.X
					+ static_cast<int>((static_cast<long long>(clamped) * layout.Track.Width) / total);
			}

			// What the bar points at: the frame being dragged to while a drag is in progress, and
			// otherwise where playback actually is - including during a seek, where watching the
			// handle sweep towards the target is the only progress there is to show.
			int DisplayFrame()
			{
				return Interaction.DraggingHandle ? Interaction.ScrubFrame : Seek::CurrentFrame();
			}

			void ActivateButton(int button)
			{
				switch (button)
				{
				case Button_JumpStart:
					Seek::RequestSeek(Seek::EarliestSeekableFrame());
					break;

				case Button_StepBack:
					// One frame back is a seek like any other: there is no way to run a frame
					// backwards, so it reloads a keyframe and runs forward to the frame before.
					if (!Seek::RequestSeek(Seek::CurrentFrame() - 1, /* pauseOnArrival: */ true))
						Controls::PrintControlMessage(L"Nothing to rewind to yet.");
					break;

				case Button_Slower:
					Controls::StepPlaybackSpeed(-1);
					break;

				case Button_PlayPause:
					Controls::TogglePlaybackPause();
					break;

				case Button_Faster:
					Controls::StepPlaybackSpeed(1);
					break;

				case Button_StepForward:
					Controls::RequestSingleStep();
					break;

				case Button_JumpEnd:
					Seek::RequestSeek(Seek::TotalFrames());
					break;

				default:
					break;
				}
			}

#pragma endregion Interaction state

#pragma region Panel drawing

			void DrawButton(const RectangleStruct& rect, int index, bool enabled)
			{
				const bool pressed = Interaction.PressedButton == index;
				const bool hovered = Interaction.HoveredButton == index;

				FillRect(rect, pressed ? ColorButtonPressed() : (hovered ? ColorButtonHover() : ColorButtonFill()));
				DrawRect(rect, ColorButtonEdge());

				const int glyph = enabled ? ColorGlyph() : ColorGlyphDim();
				const int centreX = rect.X + rect.Width / 2;
				const int centreY = rect.Y + rect.Height / 2;
				constexpr int GlyphHeight = 11;
				constexpr int GlyphWidth = 7;
				const int glyphTop = centreY - GlyphHeight / 2;

				switch (index)
				{
				case Button_JumpStart:
					FillRect({ centreX - GlyphWidth - 2, glyphTop, 2, GlyphHeight }, glyph);
					DrawTriangle(centreX - GlyphWidth + 2, glyphTop, GlyphWidth, GlyphHeight, false, glyph);
					break;

				case Button_StepBack:
					DrawTriangle(centreX - GlyphWidth / 2 - 2, glyphTop, GlyphWidth, GlyphHeight, false, glyph);
					FillRect({ centreX + GlyphWidth / 2 + 1, glyphTop, 2, GlyphHeight }, glyph);
					break;

				case Button_Slower:
					DrawTriangle(centreX - GlyphWidth - 1, glyphTop, GlyphWidth, GlyphHeight, false, glyph);
					DrawTriangle(centreX + 1, glyphTop, GlyphWidth, GlyphHeight, false, glyph);
					break;

				case Button_PlayPause:
					if (Controls::IsPlaybackPaused())
						DrawTriangle(centreX - GlyphWidth / 2, glyphTop, GlyphWidth + 2, GlyphHeight, true, glyph);
					else
						DrawPauseBars(centreX - GlyphWidth / 2, glyphTop, GlyphWidth, GlyphHeight, glyph);
					break;

				case Button_Faster:
					DrawTriangle(centreX - GlyphWidth - 1, glyphTop, GlyphWidth, GlyphHeight, true, glyph);
					DrawTriangle(centreX + 1, glyphTop, GlyphWidth, GlyphHeight, true, glyph);
					break;

				case Button_StepForward:
					FillRect({ centreX - GlyphWidth / 2 - 3, glyphTop, 2, GlyphHeight }, glyph);
					DrawTriangle(centreX - GlyphWidth / 2, glyphTop, GlyphWidth, GlyphHeight, true, glyph);
					break;

				case Button_JumpEnd:
					DrawTriangle(centreX - GlyphWidth + 1, glyphTop, GlyphWidth, GlyphHeight, true, glyph);
					FillRect({ centreX + GlyphWidth - 3, glyphTop, 2, GlyphHeight }, glyph);
					break;

				default:
					break;
				}
			}

			void FormatClock(wchar_t* buffer, size_t bufferCount, int frames, int fps)
			{
				if (fps <= 0)
					fps = 1;

				const int totalSeconds = std::max(0, frames) / fps;
				const int hours = totalSeconds / 3600;
				const int minutes = (totalSeconds / 60) % 60;
				const int seconds = totalSeconds % 60;

				if (hours > 0)
					swprintf_s(buffer, bufferCount, L"%d:%02d:%02d", hours, minutes, seconds);
				else
					swprintf_s(buffer, bufferCount, L"%d:%02d", minutes, seconds);
			}

			void DrawTrack(const PanelLayout& layout)
			{
				FillRect(layout.Track, ColorTrack());

				// Where a backwards seek can still land, which is only as far back as the oldest
				// keyframe still on disk.
				int keyframes[MaxDrawnKeyframes] = { 0 };
				const int keyframeCount = Seek::CollectKeyframeFrames(keyframes, MaxDrawnKeyframes);

				const int displayFrame = DisplayFrame();
				const int elapsedRight = TrackPositionForFrame(layout, displayFrame);

				FillRect({ layout.Track.X, layout.Track.Y, elapsedRight - layout.Track.X, layout.Track.Height },
					Interaction.DraggingHandle ? ColorScrub() : ColorElapsed());

				for (int i = 0; i < keyframeCount; ++i)
				{
					const int tickX = TrackPositionForFrame(layout, keyframes[i]);
					FillRect({ tickX, layout.Track.Y, KeyframeTickWidth, layout.Track.Height },
						ColorKeyframeTick());
				}

				DrawRect(layout.Track, ColorTrackEdge());

				const RectangleStruct handle = {
					elapsedRight - HandleWidth / 2,
					layout.Track.Y - HandleOverhang,
					HandleWidth,
					layout.Track.Height + 2 * HandleOverhang
				};

				FillRect(handle, Interaction.DraggingHandle ? ColorScrub() : ColorHandle());
				DrawRect(handle, ColorPanelEdge());
			}

#pragma endregion Panel drawing
		}

		bool ProcessMouseInput(int mouseX, int mouseY)
		{
			const bool leftDown = IsLeftButtonDown();
			const PanelLayout layout = ComputeLayout();

			if (!Controls::IsControlBarVisible() || !layout.Valid)
			{
				CancelInteraction();
				return false;
			}

			// A press that landed on the panel keeps the mouse until it is let go, wherever the
			// pointer has wandered to since: a drag off the end of the bar still belongs to the bar,
			// and a press slid off a button still has to be cancelled rather than left half-done.
			const bool holding = Interaction.DraggingHandle || Interaction.PressedButton >= 0;

			if (!holding && !Contains(layout.Panel, mouseX, mouseY))
			{
				// Tracked even from outside, so moving onto the panel with the button already down is
				// not mistaken for a press on it.
				Interaction.LeftDownLastFrame = leftDown;
				Interaction.HoveredButton = -1;
				return false;
			}

			const bool justPressed = leftDown && !Interaction.LeftDownLastFrame;
			const bool justReleased = !leftDown && Interaction.LeftDownLastFrame;
			Interaction.LeftDownLastFrame = leftDown;

			Interaction.HoveredButton = -1;
			for (int i = 0; i < ButtonCount; ++i)
			{
				if (Contains(layout.Buttons[i], mouseX, mouseY))
				{
					Interaction.HoveredButton = i;
					break;
				}
			}

			if (justPressed)
			{
				if (Interaction.HoveredButton >= 0)
				{
					Interaction.PressedButton = Interaction.HoveredButton;
				}
				else if (Contains(TrackHitArea(layout), mouseX, mouseY))
				{
					Interaction.DraggingHandle = true;
					Interaction.ScrubFrame = FrameAtTrackPosition(layout, mouseX);
				}
			}

			if (Interaction.DraggingHandle && leftDown)
				Interaction.ScrubFrame = FrameAtTrackPosition(layout, mouseX);

			if (justReleased)
			{
				if (Interaction.DraggingHandle)
				{
					const int target = Interaction.ScrubFrame;
					Interaction.DraggingHandle = false;

					// Committed on release rather than while dragging: every intermediate position
					// would otherwise be a keyframe load.
					if (!Seek::RequestSeek(target))
						Controls::PrintControlMessage(L"That part of the replay has not been watched yet.");
				}
				else if (Interaction.PressedButton >= 0
					&& Interaction.PressedButton == Interaction.HoveredButton)
				{
					// Only a press and a release on the same button counts, so sliding off one
					// cancels it the way a button is expected to.
					ActivateButton(Interaction.PressedButton);
				}

				Interaction.PressedButton = -1;
			}

			return true;
		}

		void CancelInteraction()
		{
			Interaction.PressedButton = -1;
			Interaction.DraggingHandle = false;
			Interaction.HoveredButton = -1;
			Interaction.LeftDownLastFrame = IsLeftButtonDown();
		}

		void Draw()
		{
			if (!Controls::IsControlBarVisible() || !DSurface::Composite)
				return;

			const PanelLayout layout = ComputeLayout();
			if (!layout.Valid)
				return;

			// Percent, not 0-255: DSurface::Fill_Rect_Trans clamps to 100 and scales by /100.
			FillRectTranslucent(layout.Panel, PanelFill(), 78);
			DrawRect(layout.Panel, ColorPanelEdge());

			const bool canRewind = Seek::KeyframeInterval() > 0;
			for (int i = 0; i < ButtonCount; ++i)
			{
				const bool needsRewind = (i == Button_JumpStart || i == Button_StepBack);
				DrawButton(layout.Buttons[i], i, !needsRewind || canRewind);
			}

			const int fps = Seek::RecordedFPS();
			wchar_t elapsed[16] = { 0 };
			wchar_t total[16] = { 0 };
			FormatClock(elapsed, std::size(elapsed), DisplayFrame(), fps);
			FormatClock(total, std::size(total), Seek::TotalFrames(), fps);

			wchar_t clock[40] = { 0 };
			swprintf_s(clock, L"%s / %s", elapsed, total);
			DrawTextRightAligned(clock, layout.ClockRight, ColorText());

			wchar_t speed[24] = { 0 };
			if (Seek::IsSeeking())
				swprintf_s(speed, L"seeking");
			else
				swprintf_s(speed, L"%.2fx", Controls::PlaybackSpeedMultiplier());

			DrawTextRightAligned(speed, layout.SpeedRight,
				Seek::IsSeeking() ? ColorScrub() : ColorTextDim());

			DrawTrack(layout);
		}
	}
}
