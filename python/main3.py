from PySide6.QtWidgets import (
    QApplication, QWidget, QHBoxLayout, QVBoxLayout,
    QToolButton, QFrame, QGraphicsDropShadowEffect, QLabel,
    QPushButton, QCheckBox, QComboBox, QSizePolicy, QLineEdit,
    QFileDialog, QSystemTrayIcon, QMenu, QScrollArea, QGridLayout
)
from PySide6.QtCore import Qt, QRect, QByteArray, QBuffer, QIODevice, QSize, QPropertyAnimation, QRectF, Property, QUrl, Signal, QTimer
from PySide6.QtGui import QPainter, QColor, QPen, QIcon, QBrush, QAction, QPixmap, QDesktopServices, QPainterPath
import sys
import os
import time
import json
import threading
import urllib.request

# Custom modules
from lens_logic_new import search_lens, is_server_ready
from global_hotkey_function_new import GlobalHotkey
from startup_manager import StartupManager

def get_settings_path():
    """Return persistent per-user path for settings.json."""
    if sys.platform == "win32":
        base_dir = os.environ.get("APPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Roaming")
    else:
        base_dir = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base_dir, "LensAnywhere", "settings.json")


SETTINGS_FILE = get_settings_path()


def get_resource_path(relative_path):
    """Get absolute path to resource, works for dev and for PyInstaller bundling."""
    if hasattr(sys, '_MEIPASS'):
        return os.path.join(sys._MEIPASS, relative_path)
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), relative_path)


def interpolate_color(color1, color2, factor):
    """Helper function to interpolate between two QColors."""
    r = int(color1.red() + (color2.red() - color1.red()) * factor)
    g = int(color1.green() + (color2.green() - color1.green()) * factor)
    b = int(color1.blue() + (color2.blue() - color1.blue()) * factor)
    a = int(color1.alpha() + (color2.alpha() - color1.alpha()) * factor)
    return QColor(r, g, b, a)


class ToggleSwitch(QCheckBox):
    """Custom toggle switch widget."""

    def __init__(self, parent=None,
                 track_color_off="#d3d3d3", track_color_on="#b3d4ff",
                 thumb_color_off="#ffffff", thumb_color_on="#1a73e8",
                 text=""):
        super().__init__(text, parent)
        self.setCursor(Qt.PointingHandCursor)

        self._track_color_off = QColor(track_color_off)
        self._track_color_on = QColor(track_color_on)
        self._thumb_color_off = QColor(thumb_color_off)
        self._thumb_color_on = QColor(thumb_color_on)

        self._width = 50
        self._height = 28
        self._thumb_radius = 10
        self._margin = 4

        self._position = 1.0 if self.isChecked() else 0.0

        self._animation = QPropertyAnimation(self, b"position", self)
        self._animation.setDuration(250)
        self.stateChanged.connect(self._start_animation)

    @Property(float)
    def position(self):
        return self._position

    @position.setter
    def position(self, pos):
        self._position = pos
        self.update()

    def _start_animation(self, state):
        self._animation.stop()
        self._animation.setEndValue(1.0 if state else 0.0)
        self._animation.start()

    def sizeHint(self):
        return QSize(self._width, self._height)

    def hitButton(self, pos):
        return self.rect().contains(pos)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)

        track_color = interpolate_color(self._track_color_off, self._track_color_on, self._position)
        thumb_color = interpolate_color(self._thumb_color_off, self._thumb_color_on, self._position)

        painter.setPen(Qt.NoPen)
        painter.setBrush(QBrush(track_color))

        track_rect = QRectF(0, 0, self._width, self._height)
        painter.drawRoundedRect(track_rect, self._height / 2, self._height / 2)

        travel_distance = self._width - (self._thumb_radius * 2) - (self._margin * 2)
        thumb_x = self._margin + (travel_distance * self._position)
        thumb_y = self._margin

        painter.setBrush(QBrush(QColor(0, 0, 0, 40)))
        shadow_rect = QRectF(thumb_x, thumb_y + 1, self._thumb_radius * 2, self._thumb_radius * 2)
        painter.drawEllipse(shadow_rect)

        painter.setBrush(QBrush(thumb_color))
        thumb_rect = QRectF(thumb_x, thumb_y, self._thumb_radius * 2, self._thumb_radius * 2)
        painter.drawEllipse(thumb_rect)

        if self.text():
            painter.setPen(QPen(self.palette().text().color()))
            text_rect = QRectF(self._width + 10, 0, self.width() - self._width - 10, self._height)
            painter.drawText(text_rect, Qt.AlignVCenter | Qt.AlignLeft, self.text())


class Toolbar(QFrame):
    def __init__(self, overlay_parent=None):
        super().__init__(overlay_parent)
        self.overlay = overlay_parent
        self.setObjectName("Toolbar")

        shadow = QGraphicsDropShadowEffect(self)
        shadow.setBlurRadius(15)
        shadow.setColor(QColor(0, 0, 0, 100))
        shadow.setOffset(0, 4)
        self.setGraphicsEffect(shadow)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(6, 6, 6, 6)
        layout.setSpacing(4)

        def create_btn(icon_path, fallback_text="", is_active=False):
            btn = QToolButton()
            btn.setFixedSize(36, 36)
            btn.setCursor(Qt.PointingHandCursor)
            btn.setIcon(QIcon(icon_path))
            btn.setText(fallback_text)
            btn.setIconSize(QSize(20, 20))
            if is_active:
                btn.setObjectName("ActiveBtn")
            return btn

        self.rect_btn = create_btn(get_resource_path('rect.svg'), is_active=True)

        separator = QFrame()
        separator.setFixedSize(1, 24)
        separator.setStyleSheet("background-color: #444444; margin-left: 4px; margin-right: 4px;")

        self.settings_btn = create_btn(get_resource_path('settings.svg'), "settings")
        self.settings_btn.clicked.connect(self.overlay.show_settings)

        self.cancel_btn = create_btn(get_resource_path('cancel.svg'), "✕")
        self.cancel_btn.clicked.connect(self.overlay.cancel_selection)

        layout.addWidget(self.rect_btn)
        layout.addWidget(separator)
        layout.addWidget(self.settings_btn)
        layout.addWidget(self.cancel_btn)

        self.setStyleSheet("""
            QFrame#Toolbar {
                background-color: #202020;
                border: 1px solid #333333;
                border-radius: 8px;
            }
            QToolButton {
                background-color: transparent;
                border: none;
                border-radius: 4px;
                color: #FFFFFF;
                font-size: 16px;
                font-family: 'Segoe UI Symbol', sans-serif;
            }
            QToolButton:hover {
                background-color: #333333;
            }
            QToolButton:pressed {
                background-color: #2b2b2b;
            }
            QToolButton#ActiveBtn {
                background-color: #1a73e8; 
            }
            QToolButton#ActiveBtn:hover {
                background-color: #9C72BA;
            }
        """)


import os
import threading
import urllib.request
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel, QPushButton,
    QFrame, QGridLayout, QScrollArea, QSizePolicy
)
from PySide6.QtCore import Qt, QUrl, Signal
from PySide6.QtGui import QPixmap, QPainter, QPainterPath, QColor, QDesktopServices, QIcon


class WelcomeWin(QWidget):
    """Modern Welcome & Onboarding Screen with scrolling and disclaimer verification."""

    avatar_loaded = Signal(QPixmap)

    def __init__(self, settings_win, parent=None):
        super().__init__(parent)
        self.settings_win = settings_win
        self._unlocked = False

        self.setWindowTitle("Welcome to LensAnywhere")
        self.setWindowFlag(Qt.WindowStaysOnTopHint)
        self.setWindowFlag(Qt.Window)
        self.resize(520, 680)
        self.setMinimumSize(480, 600)

        logo_path = get_resource_path("logo.png") if 'get_resource_path' in globals() else "logo.png"
        if os.path.exists(logo_path):
            self.setWindowIcon(QIcon(logo_path))

        self.setStyleSheet("""
            QWidget {
                background-color: #121214;
                color: #FFFFFF;
                font-family: 'Segoe UI Variable Display', 'Segoe UI', -apple-system, BlinkMacSystemFont, Roboto, sans-serif;
            }
            QLabel { background: transparent; }
            QLabel#Title { 
                font-size: 22px; 
                font-weight: 700; 
                color: #FFFFFF; 
                letter-spacing: -0.3px; 
            }
            QLabel#Subtitle { 
                font-size: 13px; 
                color: #38BDF8; 
                font-weight: 500; 
            }
            QLabel#SectionTitle { 
                font-size: 14px; 
                font-weight: 700; 
                color: #F4F4F5; 
                letter-spacing: -0.1px; 
            }
            QLabel#BodyText { 
                font-size: 13px; 
                color: #A1A1AA; 
                line-height: 1.45; 
            }
            QLabel#DisclaimerText { 
                font-size: 13px; 
                color: #A1A1AA; 
                line-height: 1.45; 
            }
            QLabel#FeatureTitle { 
                font-size: 13px; 
                font-weight: 600; 
                color: #FAFAFA; 
            }
            QLabel#FeatureDesc { 
                font-size: 12px; 
                color: #9A9A9E; 
                line-height: 1.35; 
            }

            QFrame#Card, QFrame#ProfileCard {
                background-color: #18181B;
                border-radius: 12px;
                border: 1px solid #27272A;
            }

            QPushButton#PrimaryBtn {
                background-color: #2563EB;
                color: #FFFFFF;
                border: none;
                border-radius: 8px;
                padding: 12px 24px;
                font-weight: 600;
                font-size: 14px;
            }
            QPushButton#PrimaryBtn:hover { background-color: #1D4ED8; }
            QPushButton#PrimaryBtn:pressed { background-color: #1E40AF; }
            QPushButton#PrimaryBtn:disabled {
                background-color: #27272A;
                color: #71717A;
                border: 1px solid #3F3F46;
            }

            QPushButton#LinkBtn {
                background-color: #27272A;
                color: #E4E4E7;
                border: 1px solid #3F3F46;
                border-radius: 6px;
                padding: 7px 14px;
                font-weight: 500;
                font-size: 12px;
            }
            QPushButton#LinkBtn:hover {
                background-color: #3F3F46;
                color: #FFFFFF;
                border-color: #52525B;
            }
        """)

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(20, 20, 20, 16)
        main_layout.setSpacing(14)

        # --- HEADER ---
        header_layout = QHBoxLayout()
        header_layout.setSpacing(14)

        lbl_icon = QLabel()
        if os.path.exists(logo_path):
            pixmap = QPixmap(logo_path).scaled(44, 44, Qt.KeepAspectRatio, Qt.SmoothTransformation)
            lbl_icon.setPixmap(pixmap)
        else:
            lbl_icon.setText("🔍")
            lbl_icon.setAlignment(Qt.AlignCenter)
            lbl_icon.setFixedSize(44, 44)
            lbl_icon.setStyleSheet("""
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #2563EB, stop:1 #0284C7);
                border-radius: 10px;
                font-size: 20px;
            """)

        title_vbox = QVBoxLayout()
        title_vbox.setSpacing(2)
        lbl_title = QLabel("LensAnywhere")
        lbl_title.setObjectName("Title")
        lbl_sub = QLabel("Seamless visual search on Windows")
        lbl_sub.setObjectName("Subtitle")

        title_vbox.addWidget(lbl_title)
        title_vbox.addWidget(lbl_sub)

        header_layout.addWidget(lbl_icon)
        header_layout.addLayout(title_vbox)
        header_layout.addStretch()

        main_layout.addLayout(header_layout)

        # --- SCROLL AREA ---
        self.scroll_area = QScrollArea()
        self.scroll_area.setWidgetResizable(True)
        self.scroll_area.setFrameShape(QFrame.NoFrame)
        self.scroll_area.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.scroll_area.setVerticalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        self.scroll_area.setStyleSheet("""
            QScrollArea {
                border: none;
                background-color: transparent;
            }
            QScrollBar:vertical {
                border: none;
                background: transparent;
                width: 5px;
                margin: 0px;
            }
            QScrollBar::handle:vertical {
                background: rgba(255, 255, 255, 0.2);
                min-height: 30px;
                border-radius: 2px;
            }
            QScrollBar::handle:vertical:hover {
                background: rgba(255, 255, 255, 0.4);
            }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
                height: 0px;
                background: none;
            }
            QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
                background: none;
            }
        """)

        scroll_content = QWidget()
        scroll_content.setStyleSheet("background-color: transparent;")
        scroll_layout = QVBoxLayout(scroll_content)
        scroll_layout.setContentsMargins(0, 0, 6, 0)
        scroll_layout.setSpacing(14)

        # 1. ABOUT CARD
        about_card = QFrame()
        about_card.setObjectName("Card")
        about_layout = QVBoxLayout(about_card)
        about_layout.setContentsMargins(20, 16, 20, 16)
        about_layout.setSpacing(8)

        lbl_about_head = QLabel("About LensAnywhere")
        lbl_about_head.setObjectName("SectionTitle")

        lbl_about_text = QLabel(
            "LensAnywhere allows you to crop any region on your screen using a custom global hotkey "
            "and instantly search it on Google Lens without third-party uploads or browser extensions."
        )
        lbl_about_text.setObjectName("BodyText")
        lbl_about_text.setWordWrap(True)

        about_layout.addWidget(lbl_about_head)
        about_layout.addWidget(lbl_about_text)
        scroll_layout.addWidget(about_card)

        # 2. PROGRAM FEATURES CARD
        features_card = QFrame()
        features_card.setObjectName("Card")
        feat_layout = QVBoxLayout(features_card)
        feat_layout.setContentsMargins(20, 16, 20, 16)
        feat_layout.setSpacing(14)

        lbl_feat_head = QLabel("Program Features")
        lbl_feat_head.setObjectName("SectionTitle")

        grid = QGridLayout()
        grid.setHorizontalSpacing(18)
        grid.setVerticalSpacing(14)

        grid.setColumnStretch(0, 1)
        grid.setColumnStretch(1, 1)

        # Standardized descriptions for balanced 2-line text rendering
        features = [
            ("✂️", "Instant Region Cropping", "Capture any screen region using a custom global hotkey."),
            ("🌐", "Direct Lens Search", "Opens search directly in your browser."),
            ("📋", "Clipboard & Auto-Save", "Optionally copy captures or save directly to a folder."),
            ("⚡", "Background Tray Utility", "Runs quietly in system tray with minimal RAM footprint.")
        ]

        for i, (icon, title, desc) in enumerate(features):
            row = i // 2
            col = i % 2

            lbl_ico = QLabel(icon)
            lbl_ico.setFixedSize(36, 36)
            lbl_ico.setAlignment(Qt.AlignCenter)
            lbl_ico.setStyleSheet("""
                background-color: #27272A;
                border: 1px solid #3F3F46;
                border-radius: 8px;
                font-size: 16px;
            """)

            box = QVBoxLayout()
            box.setSpacing(2)

            t_lbl = QLabel(title)
            t_lbl.setObjectName("FeatureTitle")

            d_lbl = QLabel(desc)
            d_lbl.setObjectName("FeatureDesc")
            d_lbl.setWordWrap(True)

            box.addWidget(t_lbl)
            box.addWidget(d_lbl)

            f_item = QHBoxLayout()
            f_item.setSpacing(10)
            f_item.setContentsMargins(0, 0, 0, 0)
            f_item.addWidget(lbl_ico, alignment=Qt.AlignTop)
            f_item.addLayout(box)

            grid.addLayout(f_item, row, col, Qt.AlignTop)

        feat_layout.addWidget(lbl_feat_head)
        feat_layout.addLayout(grid)
        scroll_layout.addWidget(features_card)

        # 3. DEVELOPER PROFILE CARD
        dev_card = QFrame()
        dev_card.setObjectName("ProfileCard")
        dev_layout = QHBoxLayout(dev_card)
        dev_layout.setContentsMargins(20, 16, 20, 16)
        dev_layout.setSpacing(14)

        self.lbl_avatar = QLabel()
        self.lbl_avatar.setFixedSize(48, 48)
        self.lbl_avatar.setPixmap(self._create_avatar_placeholder("UK"))

        dev_text_box = QVBoxLayout()
        dev_text_box.setSpacing(2)

        lbl_dev_name = QLabel("Utkarsh Kulshrestha")
        lbl_dev_name.setStyleSheet("font-size: 15px; font-weight: 700; color: #FFFFFF;")

        lbl_dev_handle = QLabel(
            '<a href="https://github.com/utkarsh05kul" style="color: #38BDF8; text-decoration: none; font-weight: 600;">@utkarsh05kul</a>'
        )
        lbl_dev_handle.setStyleSheet("font-size: 12px;")
        lbl_dev_handle.setOpenExternalLinks(True)
        lbl_dev_handle.setCursor(Qt.PointingHandCursor)

        lbl_dev_badge = QLabel("Crafted with ❤️ by Utkarsh Kulshrestha")
        lbl_dev_badge.setStyleSheet("font-size: 11px; color: #A1A1AA;")

        dev_text_box.addWidget(lbl_dev_name)
        dev_text_box.addWidget(lbl_dev_handle)
        dev_text_box.addWidget(lbl_dev_badge)

        btn_github = QPushButton("GitHub Repository 🔗")
        btn_github.setObjectName("LinkBtn")
        btn_github.setCursor(Qt.PointingHandCursor)
        btn_github.clicked.connect(self._open_github)

        dev_layout.addWidget(self.lbl_avatar)
        dev_layout.addLayout(dev_text_box)
        dev_layout.addStretch()
        dev_layout.addWidget(btn_github, alignment=Qt.AlignVCenter)

        scroll_layout.addWidget(dev_card)

        # 4. PRIVACY & LEGAL DISCLAIMER CARD
        disclaimer_card = QFrame()
        disclaimer_card.setObjectName("Card")
        disc_layout = QVBoxLayout(disclaimer_card)
        disc_layout.setContentsMargins(20, 16, 20, 16)
        disc_layout.setSpacing(8)

        lbl_disc_head = QLabel("🔒 Privacy & Legal Disclaimer")
        lbl_disc_head.setObjectName("SectionTitle")
        lbl_disc_head.setStyleSheet("font-size: 14px; font-weight: 700; color: #38BDF8;")

        lbl_disc_text = QLabel(
            "LensAnywhere does not store, collect, or transmit captured screenshots to any external server or "
            "third-party storage service. Screen captures are passed directly to Google Lens in your default browser.\n\n"
            "LensAnywhere is an independent open-source utility and is not affiliated, endorsed by, or in any way "
            "associated with Google LLC. Visual search functionality is provided entirely by Google Lens, and LensAnywhere "
            "assumes no liability for changes to Google Lens policies, search accuracy, or service availability."
        )
        lbl_disc_text.setObjectName("DisclaimerText")
        lbl_disc_text.setWordWrap(True)

        disc_layout.addWidget(lbl_disc_head)
        disc_layout.addWidget(lbl_disc_text)
        scroll_layout.addWidget(disclaimer_card)

        self.scroll_area.setWidget(scroll_content)
        main_layout.addWidget(self.scroll_area)

        # --- STICKY FOOTER BUTTON ---
        self.btn_next = QPushButton("Scroll Down")
        self.btn_next.setObjectName("PrimaryBtn")
        self.btn_next.setEnabled(False)
        self.btn_next.setCursor(Qt.PointingHandCursor)
        self.btn_next.setFixedHeight(44)
        self.btn_next.clicked.connect(self._on_next_clicked)

        main_layout.addWidget(self.btn_next)

        # Scroll position listener
        self.scroll_area.verticalScrollBar().valueChanged.connect(self._check_scroll)
        self.scroll_area.verticalScrollBar().rangeChanged.connect(self._check_scroll)

        # Connect Signal & Start Avatar Fetch
        self.avatar_loaded.connect(self._on_avatar_loaded)
        self._fetch_github_avatar()

    def _check_scroll(self):
        """Unlocks button when user scrolls down to read the disclaimer card at the end."""
        if self._unlocked:
            return
        sb = self.scroll_area.verticalScrollBar()
        if sb.maximum() == 0 or sb.value() >= sb.maximum() - 20:
            self._unlock_continue_button()

    def _unlock_continue_button(self):
        self._unlocked = True
        self.btn_next.setEnabled(True)
        self.btn_next.setText("Continue ➔")

    def _fetch_github_avatar(self):
        def fetch():
            try:
                url = "https://github.com/utkarsh05kul.png"
                req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
                with urllib.request.urlopen(req, timeout=5) as response:
                    data = response.read()
                pix = QPixmap()
                if pix.loadFromData(data):
                    rounded_pix = self._make_rounded_pixmap(pix, 48)
                    self.avatar_loaded.emit(rounded_pix)
            except Exception:
                pass

        threading.Thread(target=fetch, daemon=True).start()

    def _on_avatar_loaded(self, pixmap):
        self.lbl_avatar.setPixmap(pixmap)

    def _make_rounded_pixmap(self, pixmap, size):
        scaled = pixmap.scaled(size, size, Qt.KeepAspectRatioByExpanding, Qt.SmoothTransformation)
        out = QPixmap(size, size)
        out.fill(Qt.transparent)
        painter = QPainter(out)
        painter.setRenderHint(QPainter.Antialiasing)
        path = QPainterPath()
        path.addEllipse(0, 0, size, size)
        painter.setClipPath(path)
        painter.drawPixmap(0, 0, scaled)
        painter.end()
        return out

    def _create_avatar_placeholder(self, initials):
        pm = QPixmap(48, 48)
        pm.fill(Qt.transparent)
        painter = QPainter(pm)
        painter.setRenderHint(QPainter.Antialiasing)
        painter.setBrush(QColor("#2563EB"))
        painter.setPen(Qt.NoPen)
        painter.drawEllipse(0, 0, 48, 48)
        painter.setPen(QColor("#FFFFFF"))
        font = painter.font()
        font.setBold(True)
        font.setPixelSize(17)
        painter.setFont(font)
        painter.drawText(pm.rect(), Qt.AlignCenter, initials)
        painter.end()
        return pm

    def _open_github(self):
        QDesktopServices.openUrl(QUrl("https://github.com/utkarsh05kul/LensAnywhere-Desktop"))

    def _on_next_clicked(self):
        self.settings_win.mark_onboarding_completed()
        self.hide()
        self.settings_win.show()
        self.settings_win.raise_()
        self.settings_win.activateWindow()


class SettingsWin(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.hotkey_manager = None
        self._is_loading = False
        self._onboarding_completed = False

        self.setWindowTitle("LensAnywhere - Settings")
        self.setWindowFlag(Qt.WindowStaysOnTopHint)
        self.setWindowFlag(Qt.Window)
        self.resize(500, 600)
        self.setMinimumSize(450, 550)

        logo_path = get_resource_path("logo.png")
        if os.path.exists(logo_path):
            self.setWindowIcon(QIcon(logo_path))

        self.setStyleSheet("""
            QWidget {
                background-color: #1A1A1A;
                color: #FFFFFF;
                font-family: 'Segoe UI', -apple-system, BlinkMacSystemFont, Roboto, Arial, sans-serif;
            }
            QLabel { background: transparent; }
            QLabel#AppIcon { font-size: 32px; color: #1a73e8; font-weight: bold; }
            QLabel#AppTitle { font-size: 32px; font-weight: 800; color: #FFFFFF; }
            QLabel#PageTitle { font-size: 20px; font-weight: 600; color: #A0A0A0; margin-bottom: 10px; }
            QLabel#SettingTitle { font-size: 16px; font-weight: 600; }
            QLabel#SettingDesc { font-size: 13px; color: #888888; }

            QFrame#Card { background-color: #252525; border-radius: 12px; border: 1px solid #333333; }
            QFrame#Separator { background-color: #333333; max-height: 1px; }

            QPushButton#KeyBtn {
                background-color: #333333;
                border: 1px solid #444444;
                border-bottom: 3px solid #222222;
                border-radius: 6px;
                padding: 6px 14px;
                font-size: 13px;
                font-weight: bold;
                color: #EEEEEE;
            }
            QPushButton#KeyBtn:checked {
                background-color: #1a73e8;
                border: 1px solid #005ce6;
                border-bottom: 3px solid #0044aa;
                color: white;
            }
            QPushButton#KeyBtn:hover:!checked { background-color: #404040; }

            QComboBox#KeyDropdown {
                background-color: #333333;
                color: #FFFFFF;
                border-radius: 6px;
                padding: 6px 12px;
                font-weight: bold;
                font-size: 13px;
                border: none;
                min-width: 50px;
            }
            QComboBox#KeyDropdown:hover { background-color: #404040; }
            QComboBox#KeyDropdown::drop-down { border: none; width: 24px; }
            QComboBox#KeyDropdown QAbstractItemView {
                background-color: #2B2B2B;
                color: #FFFFFF;
                border: 1px solid #444444;
                border-radius: 6px;
                selection-background-color: #1a73e8;
                selection-color: #FFFFFF;
                outline: none;
            }

            QLineEdit#PathInput {
                background-color: #333333;
                border: 1px solid #444444;
                border-radius: 6px;
                padding: 6px 12px;
                color: #FFFFFF;
                font-size: 13px;
            }
            QPushButton#ActionBtn {
                background-color: #E0E0E0;
                color: #111111;
                border-radius: 6px;
                padding: 6px 16px;
                font-weight: bold;
                font-size: 13px;
            }
            QPushButton#ActionBtn:hover { background-color: #FFFFFF; }
            QPushButton#ActionBtn:pressed { background-color: #BDBDBD; }
        """)

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(30, 30, 30, 30)
        main_layout.setSpacing(20)

        # --- HEADER SECTION ---
        header_layout = QVBoxLayout()
        header_layout.setSpacing(2)

        title_row = QHBoxLayout()
        title_row.setSpacing(12)

        self.lbl_icon = QLabel()
        self.lbl_icon.setObjectName("AppIcon")

        if os.path.exists(logo_path):
            pixmap = QPixmap(logo_path).scaled(36, 36, Qt.KeepAspectRatio, Qt.SmoothTransformation)
            self.lbl_icon.setPixmap(pixmap)
        else:
            self.lbl_icon.setText("❖")

        lbl_app = QLabel("LensAnywhere")
        lbl_app.setObjectName("AppTitle")

        title_row.addWidget(self.lbl_icon)
        title_row.addWidget(lbl_app)
        title_row.addStretch()

        lbl_page = QLabel("Settings")
        lbl_page.setObjectName("PageTitle")

        header_layout.addLayout(title_row)
        header_layout.addWidget(lbl_page)
        main_layout.addLayout(header_layout)

        # --- HOTKEY CARD ---
        hotkey_card = QFrame()
        hotkey_card.setObjectName("Card")
        hotkey_layout = QVBoxLayout(hotkey_card)
        hotkey_layout.setContentsMargins(20, 20, 20, 20)
        hotkey_layout.setSpacing(15)

        self.hotkey_s = ToggleSwitch()
        self.hotkey_s.setChecked(True)
        hotkey_row1 = self._create_setting_row("Global Hotkey", "Enable or disable the quick-launch shortcut.",
                                               self.hotkey_s)
        hotkey_layout.addLayout(hotkey_row1)

        keys_layout = QHBoxLayout()
        keys_layout.setSpacing(8)

        self.ctrl_btn = self._create_key_btn("Ctrl")
        self.shift_btn = self._create_key_btn("Shift")
        self.win_btn = self._create_key_btn("Win")
        self.alt_btn = self._create_key_btn("Alt")

        self.ctrl_btn.setChecked(True)
        self.shift_btn.setChecked(True)
        self.alt_btn.setChecked(True)

        keys_layout.addWidget(self.ctrl_btn)
        keys_layout.addWidget(self.shift_btn)
        keys_layout.addWidget(self.win_btn)
        keys_layout.addWidget(self.alt_btn)
        keys_layout.addStretch()

        self.key_dropdown = QComboBox()
        self.key_dropdown.setObjectName("KeyDropdown")
        self.key_dropdown.setCursor(Qt.PointingHandCursor)
        self.key_dropdown.addItems([chr(i) for i in range(ord('A'), ord('Z') + 1)])
        self.key_dropdown.setCurrentText('S')

        keys_layout.addWidget(self.key_dropdown)
        hotkey_layout.addLayout(keys_layout)
        main_layout.addWidget(hotkey_card)

        # --- GENERAL SETTINGS CARD ---
        general_card = QFrame()
        general_card.setObjectName("Card")
        general_layout = QVBoxLayout(general_card)
        general_layout.setContentsMargins(20, 20, 20, 20)
        general_layout.setSpacing(15)

        # Clipboard Setting (Default OFF)
        self.clipboard_s = ToggleSwitch()
        self.clipboard_s.setChecked(False)
        clip_row = self._create_setting_row("Save to Clipboard", "Automatically copy the result to your clipboard.",
                                            self.clipboard_s)
        general_layout.addLayout(clip_row)

        # Save to file row
        loc_layout = QHBoxLayout()
        self.loc_input = QLineEdit()
        self.loc_input.setObjectName("PathInput")
        self.loc_input.setReadOnly(True)
        self.loc_input.setPlaceholderText("Select save location (Optional)...")

        self.loc_btn = QPushButton("Browse")
        self.loc_btn.setObjectName("ActionBtn")
        self.loc_btn.setCursor(Qt.PointingHandCursor)
        self.loc_btn.clicked.connect(self._browse_location)

        loc_layout.addWidget(self.loc_input)
        loc_layout.addWidget(self.loc_btn)

        loc_label = QLabel("Auto-Save Captures To:")
        loc_label.setObjectName("SettingTitle")

        general_layout.addWidget(loc_label)
        general_layout.addLayout(loc_layout)

        separator = QFrame()
        separator.setObjectName("Separator")
        general_layout.addWidget(separator)

        # Startup Setting (Default ON)
        self.startup_s = ToggleSwitch()
        self.startup_s.setChecked(True)
        startup_row = self._create_setting_row("Launch on Startup",
                                               "Start LensAnywhere silently in the background on boot.", self.startup_s)
        general_layout.addLayout(startup_row)

        main_layout.addWidget(general_card)

        main_layout.addStretch()

        # Footer Credit
        lbl_about = QLabel(
            'LensAnywhere v1.0 • Created by <a href="https://github.com/utkarsh05kul" '
            'style="color: #60A5FA; text-decoration: none; font-weight: 600;">Utkarsh Kulshrestha</a>'
        )
        lbl_about.setObjectName("SettingDesc")
        lbl_about.setAlignment(Qt.AlignCenter)
        lbl_about.setOpenExternalLinks(True)
        lbl_about.setCursor(Qt.PointingHandCursor)
        main_layout.addWidget(lbl_about)

        # --- SIGNALS ---
        self.hotkey_s.toggled.connect(self._on_master_toggle)
        self.ctrl_btn.clicked.connect(self._on_hotkey_changed)
        self.shift_btn.clicked.connect(self._on_hotkey_changed)
        self.alt_btn.clicked.connect(self._on_hotkey_changed)
        self.win_btn.clicked.connect(self._on_hotkey_changed)
        self.key_dropdown.currentTextChanged.connect(self._on_hotkey_changed)
        self.clipboard_s.toggled.connect(self._on_clipboard_toggled)
        self.startup_s.toggled.connect(self._on_startup_toggled)

        self.load_settings()

    def set_hotkey_manager(self, manager):
        self.hotkey_manager = manager

    def is_onboarding_completed(self):
        return self._onboarding_completed

    def mark_onboarding_completed(self):
        self._onboarding_completed = True
        self.save_settings()

    def get_current_hotkey_string(self):
        parts = []
        if self.ctrl_btn.isChecked(): parts.append("Ctrl")
        if self.shift_btn.isChecked(): parts.append("Shift")
        if self.alt_btn.isChecked(): parts.append("Alt")
        if self.win_btn.isChecked(): parts.append("Win")
        parts.append(self.key_dropdown.currentText())
        return "+".join(parts)

    def _browse_location(self):
        directory = QFileDialog.getExistingDirectory(self, "Select Save Directory")
        if directory:
            self.loc_input.setText(directory)
            self.save_settings()

    def _on_clipboard_toggled(self, checked):
        if getattr(self, "_is_loading", False):
            return

        if checked and not self.loc_input.text().strip():
            self._browse_location()

        self.save_settings()

    def _on_startup_toggled(self, checked):
        if getattr(self, "_is_loading", False):
            return

        StartupManager.set_enabled(checked)
        self.save_settings()

    def _on_master_toggle(self, checked):
        if getattr(self, "_is_loading", False):
            return

        if self.hotkey_manager:
            self.hotkey_manager.set_enabled(checked and is_server_ready())
        self.save_settings()

    def _on_hotkey_changed(self):
        if getattr(self, "_is_loading", False):
            return

        new_hotkey = self.get_current_hotkey_string()
        if self.hotkey_manager:
            self.hotkey_manager.update_hotkey(new_hotkey)
        self.save_settings()

    def save_settings(self):
        if getattr(self, "_is_loading", False):
            return

        data = {
            "onboarding_completed": self._onboarding_completed,
            "hotkey_enabled": self.hotkey_s.isChecked(),
            "ctrl": self.ctrl_btn.isChecked(),
            "shift": self.shift_btn.isChecked(),
            "win": self.win_btn.isChecked(),
            "alt": self.alt_btn.isChecked(),
            "key": self.key_dropdown.currentText(),
            "save_to_clipboard": self.clipboard_s.isChecked(),
            "save_location": self.loc_input.text(),
            "launch_on_startup": self.startup_s.isChecked()
        }
        try:
            settings_dir = os.path.dirname(SETTINGS_FILE)
            if settings_dir:
                os.makedirs(settings_dir, exist_ok=True)
            tmp_path = SETTINGS_FILE + ".tmp"
            with open(tmp_path, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=4)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp_path, SETTINGS_FILE)
        except Exception as e:
            print(f"Error saving settings: {e}")

    def _apply_default_settings(self):
        self._onboarding_completed = False
        self.clipboard_s.setChecked(False)
        self.startup_s.setChecked(True)
        StartupManager.set_enabled(True)

    def load_settings(self):
        self._is_loading = True
        if os.path.exists(SETTINGS_FILE):
            try:
                with open(SETTINGS_FILE, "r", encoding="utf-8") as f:
                    data = json.load(f)
                if not isinstance(data, dict):
                    raise ValueError("Invalid settings format: expected JSON object")

                self._onboarding_completed = bool(data.get("onboarding_completed", False))
                self.hotkey_s.setChecked(bool(data.get("hotkey_enabled", True)))
                self.ctrl_btn.setChecked(bool(data.get("ctrl", True)))
                self.shift_btn.setChecked(bool(data.get("shift", True)))
                self.win_btn.setChecked(bool(data.get("win", False)))
                self.alt_btn.setChecked(bool(data.get("alt", True)))

                key = data.get("key", "S")
                if isinstance(key, str) and key.upper() in [chr(i) for i in range(ord('A'), ord('Z') + 1)]:
                    self.key_dropdown.setCurrentText(key.upper())

                self.clipboard_s.setChecked(bool(data.get("save_to_clipboard", False)))
                save_location = data.get("save_location", "")
                self.loc_input.setText(save_location if isinstance(save_location, str) else "")

                startup_enabled = bool(data.get("launch_on_startup", True))
                self.startup_s.setChecked(startup_enabled)
                StartupManager.set_enabled(startup_enabled)

            except Exception as e:
                print(f"Error loading settings: {e}")
                self._apply_default_settings()
        else:
            self._apply_default_settings()

        self._is_loading = False

    def _create_setting_row(self, title, description, widget):
        row = QHBoxLayout()
        text_layout = QVBoxLayout()
        text_layout.setSpacing(2)

        lbl_title = QLabel(title)
        lbl_title.setObjectName("SettingTitle")
        text_layout.addWidget(lbl_title)

        if description:
            lbl_desc = QLabel(description)
            lbl_desc.setObjectName("SettingDesc")
            lbl_desc.setWordWrap(True)
            text_layout.addWidget(lbl_desc)

        row.addLayout(text_layout)
        row.addStretch()
        widget.setSizePolicy(QSizePolicy.Fixed, QSizePolicy.Fixed)
        row.addWidget(widget, alignment=Qt.AlignVCenter)
        return row

    def _create_key_btn(self, text):
        btn = QPushButton(text)
        btn.setObjectName("KeyBtn")
        btn.setCheckable(True)
        btn.setCursor(Qt.PointingHandCursor)
        btn.setFixedSize(60, 32)
        return btn


class Overlay(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowFlag(Qt.WindowStaysOnTopHint)
        self.setWindowFlag(Qt.FramelessWindowHint)
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setGeometry(QApplication.primaryScreen().geometry())

        self.tray_icon = None

        self.drawing = False
        self.start_pos = None
        self.current_pos = None
        self.end_pos = None
        self.box = None

        self.pen = QPen(QColor(255, 255, 255, 220))
        self.pen.setWidth(2)
        self.pen.setStyle(Qt.DashLine)

        self.toolbar = Toolbar(self)
        self.settings = SettingsWin(self)

    def set_tray_icon(self, tray_icon):
        self.tray_icon = tray_icon

    def trigger_capture(self):
        """Triggers overlay only if tunnel is active."""
        if not is_server_ready():
            if self.tray_icon:
                self.tray_icon.showMessage(
                    "LensAnywhere",
                    "Cloudflare Tunnel initializing... Please wait a few seconds.",
                    QSystemTrayIcon.Warning,
                    3000
                )
            return
        self.showFullScreen()

    def show_settings(self):
        self.settings.show()
        self.settings.raise_()
        self.settings.activateWindow()

    def showEvent(self, event):
        self.reset()
        super().showEvent(event)
        self.toolbar.adjustSize()
        tb_width = self.toolbar.width()
        x = (self.width() - tb_width) // 2
        y = 20
        self.toolbar.move(x, y)
        self.toolbar.show()

    def pixmap_to_png_bytes(self, pixmap):
        byte_array = QByteArray()
        buffer = QBuffer(byte_array)
        buffer.open(QIODevice.WriteOnly)
        pixmap.save(buffer, "PNG")
        buffer.close()
        return bytes(byte_array)

    def capture_region(self, rect):
        if not rect or rect.width() <= 0 or rect.height() <= 0:
            self.reset()
            self.hide()
            return

        self.reset()
        self.hide()
        QApplication.processEvents()

        screen = QApplication.primaryScreen()
        pixmap = screen.grabWindow(0, rect.x(), rect.y(), rect.width(), rect.height())

        saved_info_lines = []

        if self.settings.clipboard_s.isChecked():
            QApplication.clipboard().setPixmap(pixmap)
            saved_info_lines.append("✓ Copied to Clipboard")

        save_path = self.settings.loc_input.text()
        if save_path and os.path.isdir(save_path):
            filename = f"LensAnywhere_{int(time.time())}.png"
            filepath = os.path.join(save_path, filename)
            pixmap.save(filepath, "PNG")
            saved_info_lines.append(f"✓ Saved to {filepath}")
        else:
            saved_info_lines.append("ℹ Not saved to local disk")

        if self.tray_icon:
            notify_msg = "Opening search in Google Lens...\n" + "\n".join(saved_info_lines)
            self.tray_icon.showMessage("LensAnywhere", notify_msg, QSystemTrayIcon.Information, 3500)

        Bdata = self.pixmap_to_png_bytes(pixmap)
        threading.Thread(target=search_lens, args=(Bdata,), daemon=True).start()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(0, 0, 0, 140))

        if self.drawing and self.start_pos is not None and self.current_pos is not None:
            painter.setPen(self.pen)
            rect = QRect(self.start_pos, self.current_pos).normalized()
            painter.setCompositionMode(QPainter.CompositionMode_Clear)
            painter.fillRect(rect, Qt.transparent)
            painter.setCompositionMode(QPainter.CompositionMode_SourceOver)
            painter.drawRect(rect)

    def mousePressEvent(self, event):
        if not self.toolbar.geometry().contains(event.position().toPoint()):
            self.drawing = True
            self.toolbar.hide()
            self.current_pos = event.position().toPoint()
            self.start_pos = self.current_pos
            self.repaint()

    def mouseMoveEvent(self, event):
        if self.drawing:
            self.current_pos = event.position().toPoint()
            self.update()

    def mouseReleaseEvent(self, event):
        if not self.drawing:
            return

        self.drawing = False
        self.end_pos = event.position().toPoint()

        if self.start_pos is not None and self.end_pos is not None:
            self.box = QRect(self.start_pos, self.end_pos).normalized()
            if self.box.width() > 5 and self.box.height() > 5:
                self.capture_region(self.box)
            else:
                self.toolbar.show()
                self.box = None
        else:
            self.toolbar.show()

        self.current_pos = None
        self.start_pos = None

    def reset(self):
        self.drawing = False
        self.start_pos = None
        self.current_pos = None
        self.end_pos = None
        self.box = None
        self.repaint()

    def cancel_selection(self):
        self.reset()
        self.hide()


def create_tray_icon_pixmap():
    pm = QPixmap(64, 64)
    pm.fill(Qt.transparent)
    painter = QPainter(pm)
    painter.setRenderHint(QPainter.Antialiasing)
    painter.setBrush(QColor("#1a73e8"))
    painter.setPen(Qt.NoPen)
    painter.drawEllipse(4, 4, 56, 56)
    painter.setPen(QPen(Qt.white, 6))
    painter.setBrush(Qt.NoBrush)
    painter.drawEllipse(18, 18, 28, 28)
    painter.end()
    return pm


if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)

    overlay = Overlay()
    initial_hotkey = overlay.settings.get_current_hotkey_string()

    hotkey_manager = GlobalHotkey(
        app,
        overlay,
        initial_hotkey
    )

    overlay.settings.set_hotkey_manager(hotkey_manager)

    # Disable hotkey until Cloudflare tunnel is confirmed ready
    hotkey_manager.set_enabled(False)

    logo_file = get_resource_path("logo.png")
    if os.path.exists(logo_file):
        tray_icon = QSystemTrayIcon(QIcon(logo_file), app)
    else:
        tray_icon = QSystemTrayIcon(QIcon(create_tray_icon_pixmap()), app)

    tray_icon.setToolTip("LensAnywhere (Connecting Tunnel...)")
    overlay.set_tray_icon(tray_icon)

    tray_menu = QMenu()

    action_capture = QAction("Capture Screen", app)
    action_capture.triggered.connect(overlay.trigger_capture)

    action_settings = QAction("Settings", app)
    action_settings.triggered.connect(overlay.show_settings)

    action_exit = QAction("Exit LensAnywhere", app)
    action_exit.triggered.connect(app.quit)

    tray_menu.addAction(action_capture)
    tray_menu.addAction(action_settings)
    tray_menu.addSeparator()
    tray_menu.addAction(action_exit)

    tray_icon.setContextMenu(tray_menu)
    tray_icon.show()

    def on_tray_activated(reason):
        if reason == QSystemTrayIcon.DoubleClick:
            overlay.trigger_capture()

    tray_icon.activated.connect(on_tray_activated)

    # Poller timer to activate hotkey once Cloudflare tunnel is ready
    def check_tunnel_status():
        if is_server_ready():
            hotkey_manager.set_enabled(overlay.settings.hotkey_s.isChecked())
            tray_icon.setToolTip("LensAnywhere (Ready)")
            tunnel_timer.stop()

    tunnel_timer = QTimer()
    tunnel_timer.timeout.connect(check_tunnel_status)
    tunnel_timer.start(500)

    print("LensAnywhere started successfully")
    print("Application running in System Tray ---")

    if not overlay.settings.is_onboarding_completed():
        welcome_win = WelcomeWin(settings_win=overlay.settings)
        welcome_win.show()

    sys.exit(app.exec())