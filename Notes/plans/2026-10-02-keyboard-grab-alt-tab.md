# Graph view keyboard grab blocks Alt+Tab on Linux (GitLab #305)

**Status:** Implemented 2026-10-09, after the 2.1.0 release, as planned below. Two
refinements: read-only text widgets (e.g. a `QTextBrowser`) do not block hover focus, and
`mousePressEvent` clears the saved widget so focus given by a click stays on the graph.
Awaiting Linux X11 confirmation.

## Symptom

On Linux (reported on Debian 13 with Cinnamon, which is X11), Alt+Tab does nothing while
the mouse is over OSCAR. It starts working again after clicking outside the window. With
OSCAR maximized, the user has no way out. The bug does not reproduce on Windows.

## Root cause

`gGraphView::mouseMoveEvent()` calls `grabKeyboard()` every time the mouse moves
(`oscar/Graphs/gGraphView.cpp`, top of the function). The grab is released only in:

- `gGraphView::leaveEvent()`, which calls `releaseKeyboard()`
- `Daily::on_ReloadDay()`, which calls `graphView()->releaseKeyboard()`

On xcb, `QWidget::grabKeyboard()` becomes an X server keyboard grab (`xcb_grab_keyboard`).
While OSCAR holds it, the window manager receives no keystrokes, so Alt+Tab is lost. On
Windows, the grab only redirects keys inside the application, and the OS still handles
Alt+Tab. With the window maximized, the graphs cover most of the screen, so `leaveEvent`
rarely fires.

Why the grab exists: it makes keys reach the graph whenever the mouse hovers over it, with
no click needed first. Those keys are arrows, PgUp/PgDn, Esc (undo zoom), F3 (line cursor),
F1, and Alt-select/Alt+B (add bookmark). See `gGraphView::keyPressEvent` and
`keyReleaseEvent`.

Open question: 1.7.1 has identical grab/release code, yet the reporter says 1.x did not
behave this way. The difference is not in OSCAR code. It may come from the Qt5→Qt6 xcb
plugin or from how the reporter installed 1.x. This has not been verified.

## Planned fix

Replace the X-level grab with ordinary focus, so the cross-platform behaviour stays the
same:

1. In `mouseMoveEvent`, remove `grabKeyboard()`. Instead, if `isActiveWindow()` and
   `!hasFocus()`:
   - Do not take focus if `QApplication::focusWidget()` is a text-entry widget
     (`QLineEdit`, `QAbstractSpinBox`, `QTextEdit`, `QPlainTextEdit`, or an editable
     `QComboBox`).
   - Otherwise, save the current focus widget in a `QPointer<QWidget>` member and call
     `setFocus(Qt::MouseFocusReason)`.
2. In `leaveEvent`, replace `releaseKeyboard()` with this: if `hasFocus()` and the saved
   widget is still valid, visible and enabled, give focus back to it. Then clear the saved
   pointer.
3. In `Daily::on_ReloadDay()`, remove `graphView()->releaseKeyboard()`, since nothing
   grabs any more.
4. Check that no other `grabKeyboard`/`releaseKeyboard` call remains in `oscar/`
   (excluding thirdparty).

## Risks considered

- **Fields that act on focus loss:** the Daily weight spin box (re-saves the weight and
  refreshes Overview) and the bookmark filter `editingFinished` (would apply a half-typed
  filter). Both are avoided by step 1, which leaves text-entry widgets alone.
- **Selection loss:** a line edit drops its selection when it loses focus. Also avoided by
  the text-entry exclusion.
- **Highlight flicker:** the calendar and bookmark table switch to their inactive selection
  colour while the graph has focus. This is cosmetic, but check it.
- **Keys after leaving:** when nothing had focus before, the graph keeps focus after the
  mouse leaves, so arrows and Esc still go to the graph. Today they stop.
- **Intended behaviour change:** when a notes or weight field has focus, hovering a graph
  no longer sends keys to the graph. Today, typing with the mouse over a graph sends the
  keys to the graph.
- **Not affected:** menu and window-level shortcuts, and the menu bar's Alt handling (it
  uses an application-wide event filter).
- **Shared code:** Overview, oximetry import and detached graph windows all use this code,
  so they get the same change.

## Testing

- **Windows (regression):** on Daily, Overview and a detached graph, while hovering
  without clicking, check arrows, PgUp/PgDn, Esc, F3, Alt-drag select and Alt+B. Typing in
  notes, the weight box and the bookmark filter must not be interrupted by mouse movement.
  Check calendar arrow-key navigation after the mouse crosses the graphs.
- **Linux X11 (the fix itself):** with OSCAR maximized and the mouse over the graphs,
  Alt+Tab must switch windows. Ask the reporter, or another X11 user, to test a build.
- **macOS:** quick check of hover keys.
