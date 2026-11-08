"""The rules for "every active microphone" (stage 10q), without COM so they can be tested.

An endpoint is anything with GetMute() and SetMute(value, ctx). One that fails (unplugged a
moment ago) is skipped, the others still count.
"""

NONE, OPEN, MUTED = 0, 1, 2


def state_of(endpoints):
    """MUTED only when every microphone that answers is muted."""
    seen = False
    for ep in endpoints:
        try:
            if not ep.GetMute():
                return OPEN
            seen = True
        except Exception:  # noqa: BLE001 - also comtypes.COMError
            continue
    return MUTED if seen else NONE


def toggle(endpoints):
    """One open: mute them all. All muted: open them all. None: nothing."""
    state = state_of(endpoints)
    if state == NONE:
        return
    value = 1 if state == OPEN else 0
    for ep in endpoints:
        try:
            ep.SetMute(value, None)
        except Exception:  # noqa: BLE001
            continue


def vol_payload(level, muted, mic_state, send_volume):
    """The PR:VOL line. With the setting off the card gets a neutral value and no mic."""
    if not send_volume:
        return {"v": 50, "m": False}
    return {"v": level, "m": muted, "mic": mic_state}
