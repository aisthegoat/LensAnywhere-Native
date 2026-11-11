import os
import sys

# Windows registry module (built into Python standard library)
if sys.platform == "win32":
    import winreg
else:
    winreg = None


class StartupManager:
    APP_NAME = "LensAnywhere"

    @classmethod
    def get_command(cls) -> str:
        """
        Returns the command string used to boot OpenLens.
        Handles both compiled .exe execution and raw .py execution.
        """
        if getattr(sys, 'frozen', False):
            # Running as a compiled PyInstaller .exe
            return f'"{sys.executable}"'
        else:
            # Running as a Python script (.py)
            main_file = os.path.abspath(sys.argv[0])
            python_exe = sys.executable

            # Use pythonw.exe if available so no command console pops up on boot
            if python_exe.endswith("python.exe"):
                pythonw = os.path.join(os.path.dirname(python_exe), "pythonw.exe")
                if os.path.exists(pythonw):
                    python_exe = pythonw

            return f'"{python_exe}" "{main_file}"'

    @classmethod
    def set_enabled(cls, enable: bool) -> bool:
        """Enables or disables launch on Windows boot via Registry."""
        if sys.platform != "win32" or winreg is None:
            return False

        key_path = r"Software\Microsoft\Windows\CurrentVersion\Run"
        try:
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path, 0, winreg.KEY_ALL_ACCESS)
            if enable:
                cmd = cls.get_command()
                winreg.SetValueEx(key, cls.APP_NAME, 0, winreg.REG_SZ, cmd)
                print(f"[StartupManager] Registered boot command: {cmd}")
            else:
                try:
                    winreg.DeleteValue(key, cls.APP_NAME)
                    print("[StartupManager] Removed from Windows startup.")
                except FileNotFoundError:
                    pass  # Was not registered
            winreg.CloseKey(key)
            return True
        except Exception as e:
            print(f"[StartupManager] Error updating registry: {e}")
            return False

    @classmethod
    def is_enabled(cls) -> bool:
        """Checks if OpenLens is currently set to launch on boot in Windows Registry."""
        if sys.platform != "win32" or winreg is None:
            return False

        key_path = r"Software\Microsoft\Windows\CurrentVersion\Run"
        try:
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path, 0, winreg.KEY_READ)
            try:
                val, _ = winreg.QueryValueEx(key, cls.APP_NAME)
                winreg.CloseKey(key)
                return bool(val)
            except FileNotFoundError:
                winreg.CloseKey(key)
                return False
        except Exception:
            return False