import ctypes
from ctypes import wintypes

from PySide6.QtCore import (
    Qt,
    QObject,
    QEvent,
    QAbstractNativeEventFilter,
)

# ---------------- Windows Constants ----------------

WM_HOTKEY = 0x0312

MODIFIERS = {
    "ALT": 0x0001,
    "CTRL": 0x0002,
    "CONTROL": 0x0002,
    "SHIFT": 0x0004,
    "WIN": 0x0008,
}


class _HotkeyFilter(QAbstractNativeEventFilter):

    def __init__(self, manager):
        super().__init__()
        self.manager = manager

    def nativeEventFilter(self, eventType, message):
        msg = wintypes.MSG.from_address(int(message))

        if (
                msg.message == WM_HOTKEY
                and msg.wParam == self.manager.hotkey_id
        ):
            self.manager.show_window()
            return True, 0

        return False, 0


class _WindowEventFilter(QObject):

    def __init__(self, manager):
        super().__init__()
        self.manager = manager

    def eventFilter(self, obj, event):
        if (
                self.manager.hide_on_escape
                and event.type() == QEvent.KeyPress
                and event.key() == Qt.Key_Escape
        ):
            obj.reset()
            obj.hide()
            return True

        if (
                self.manager.hide_on_close
                and event.type() == QEvent.Close
        ):
            event.ignore()
            obj.hide()
            return True

        return False


class GlobalHotkey:
    _next_id = 1

    def __init__(
            self,
            app,
            window,
            hotkey="Ctrl+Shift+Alt+S",
            hide_on_escape=True,
            hide_on_close=True,
            activate=True,
            raise_window=True,
            focus=True,
    ):
        self.app = app
        self.window = window

        self.hide_on_escape = hide_on_escape
        self.hide_on_close = hide_on_close

        self.activate = activate
        self.raise_window = raise_window
        self.focus = focus

        self.user32 = ctypes.windll.user32

        self.hotkey_id = GlobalHotkey._next_id
        GlobalHotkey._next_id += 1

        self.current_hotkey = hotkey
        self.is_registered = False

        self._register(self.current_hotkey)

        self.native_filter = _HotkeyFilter(self)
        app.installNativeEventFilter(self.native_filter)

        self.window_filter = _WindowEventFilter(self)
        window.installEventFilter(self.window_filter)

        app.aboutToQuit.connect(self.unregister)

    def _register(self, hotkey_str):
        if not hotkey_str:
            return False

        try:
            modifiers, vk = self._parse_hotkey(hotkey_str)
            success = self.user32.RegisterHotKey(None, self.hotkey_id, modifiers, vk)
            if success:
                self.is_registered = True
                print(f"Hotkey registered successfully: {hotkey_str}")
            else:
                print(f"Warning: Couldn't register hotkey: {hotkey_str} (It might be used by another app)")
            return success
        except Exception as e:
            print(f"Failed to register hotkey {hotkey_str}: {e}")
            return False

    def unregister(self):
        if self.is_registered:
            self.user32.UnregisterHotKey(None, self.hotkey_id)
            self.is_registered = False
            print(f"Hotkey unregistered.")

    def update_hotkey(self, new_hotkey_str):
        """Allows changing the hotkey on the fly."""
        self.unregister()
        self.current_hotkey = new_hotkey_str
        return self._register(new_hotkey_str)

    def set_enabled(self, enabled):
        """Turns the hotkey listener completely on or off."""
        if enabled:
            if not self.is_registered:
                self._register(self.current_hotkey)
        else:
            self.unregister()

    def show_window(self):
        self.window.show()

        if self.raise_window:
            self.window.raise_()

        if self.activate:
            self.window.activateWindow()

        if self.focus:
            self.window.setFocus()

    @staticmethod
    def _parse_hotkey(text):
        parts = text.upper().split("+")
        modifiers = 0
        key = None

        for part in parts:
            part = part.strip()
            if part in MODIFIERS:
                modifiers |= MODIFIERS[part]
            else:
                key = part

        if key is None:
            raise ValueError("No key specified.")

        if len(key) == 1:
            return modifiers, ord(key)

        if key.startswith("F"):
            number = int(key[1:])
            return modifiers, 0x70 + number - 1

        raise ValueError(f"Unsupported key: {key}")