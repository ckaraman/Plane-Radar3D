"""The menu bar icon on a Mac (what the notification area icon is on Windows)."""

import objc
from AppKit import (NSApplication, NSImage, NSMenu, NSMenuItem, NSStatusBar, NSTimer,
                    NSVariableStatusItemLength)
from Foundation import NSData, NSObject
from PyObjCTools import AppHelper


class _Target(NSObject):
    def initWithCallbacks_(self, cbs):
        self = objc.super(_Target, self).init()
        if self is None:
            return None
        self.cbs = cbs
        return self

    def edit_(self, _sender):
        self.cbs["edit"]()

    def release_(self, _sender):
        self.cbs["release"]()

    def quit_(self, _sender):
        self.cbs["quit"]()

    def tick_(self, _timer):
        self.cbs["tick"]()


class MacTray:
    """Created on the main thread before the window loop starts."""

    def __init__(self, png, status_text, released, on_edit, on_release, on_quit):
        NSApplication.sharedApplication().setActivationPolicy_(1)  # accessory: no Dock icon
        self._status_text, self._released = status_text, released
        self.target = _Target.alloc().initWithCallbacks_(
            {"edit": on_edit, "release": on_release, "quit": on_quit, "tick": self.refresh})
        self.item = NSStatusBar.systemStatusBar().statusItemWithLength_(NSVariableStatusItemLength)
        img = NSImage.alloc().initWithData_(NSData.dataWithBytes_length_(png, len(png)))
        img.setSize_((18, 18))
        img.setTemplate_(True)  # drawn in the menu bar's own colour (light / dark)
        self.item.button().setImage_(img)
        self.item.button().setToolTip_("Plane Radar Deck")
        menu = NSMenu.alloc().init()
        menu.setAutoenablesItems_(False)
        self.status = NSMenuItem.alloc().initWithTitle_action_keyEquivalent_("Kart bekleniyor…", None, "")
        self.status.setEnabled_(False)
        menu.addItem_(self.status)
        edit = NSMenuItem.alloc().initWithTitle_action_keyEquivalent_("Tuşları düzenle…", "edit:", "")
        edit.setTarget_(self.target)
        menu.addItem_(edit)
        self.rel = NSMenuItem.alloc().initWithTitle_action_keyEquivalent_(
            "Bağlantıyı bırak (kurulum sayfası için)", "release:", "")
        self.rel.setTarget_(self.target)
        menu.addItem_(self.rel)
        menu.addItem_(NSMenuItem.separatorItem())
        quit_ = NSMenuItem.alloc().initWithTitle_action_keyEquivalent_("Programı kapat", "quit:", "")
        quit_.setTarget_(self.target)
        menu.addItem_(quit_)
        self.item.setMenu_(menu)
        self.timer = NSTimer.scheduledTimerWithTimeInterval_target_selector_userInfo_repeats_(
            2.0, self.target, "tick:", None, True)
        self.refresh()

    def refresh(self):
        try:
            self.status.setTitle_(self._status_text())
            self.rel.setState_(1 if self._released() else 0)
        except Exception:  # noqa: BLE001
            pass

    def stop(self):
        try:
            NSStatusBar.systemStatusBar().removeStatusItem_(self.item)
        except Exception:  # noqa: BLE001
            pass


def activate():
    """Bring our window in front of other apps (an accessory app is not activated on its own)."""
    AppHelper.callAfter(lambda: NSApplication.sharedApplication().activateIgnoringOtherApps_(True))
