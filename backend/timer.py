"""Kitchen-timer voice intent: parse, track, phrase.

Deterministic (no LLM): a matched "set a timer for X minutes" is answered and
executed without a model round-trip. The device owns the countdown display and
the end-of-timer beep (via FRAME_TIMER_SET), so it keeps counting even if the
network or backend dies mid-timer. This module keeps only a best-effort mirror
(end timestamp per device) so Charlie can answer "how much is left on the
timer" without waking anything.
"""
from __future__ import annotations

import re
import time
import unicodedata

# device_id -> monotonic end timestamp of the active timer
_ACTIVE: dict[int, float] = {}

_NUMWORDS = {
    # English
    "one": 1, "a": 1, "an": 1, "two": 2, "three": 3, "four": 4, "five": 5,
    "six": 6, "seven": 7, "eight": 8, "nine": 9, "ten": 10, "eleven": 11,
    "twelve": 12, "fifteen": 15, "twenty": 20, "thirty": 30, "fourty": 40,
    "forty": 40, "fortyfive": 45, "sixty": 60,
    # Slovak (diacritics folded)
    "jedna": 1, "jeden": 1, "dva": 2, "dve": 2, "tri": 3, "styri": 4,
    "styri": 4, "pat": 5, "sest": 6, "sedem": 7, "osem": 8, "devat": 9,
    "desat": 10, "dvanast": 12, "patnast": 15, "sestnast": 16,
    "sedemnast": 17, "osemnast": 18, "devatnast": 19, "dvadsat": 20,
    "dvadsiat": 20, "tridsat": 30, "tridsiat": 30, "patsat": 50, "hodnu": 1,
}

_UNIT = (r"(?:sec(?:onds?|undy?)?|sekundy?|sek|secs?|"
         r"mins?|minutes?|minuty|minut(?:a|u|y|e)?|min|"
         r"hours?|hrs?|hodiny|hodinu|hodin(?:y|u|a)?|hod)")

_NUM = r"(?:\d{1,3}|(?:one|a|an|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|fifteen|twenty|thirty|fourty|forty|sixty|jedna|jeden|dva|dve|tri|styri|styri|pat|sest|sedem|osem|devat|desat|dvanast|patnast|sestnast|sedemnast|osemnast|devatnast|dvadsat|dvadsiat|tridsat|tridsiat|patsat)\b)\s*" + _UNIT

_TIMERWORD = r"\b(?:timer|countdown|casovac|casovana|casovane|casovani|casovaci|hodiny\b.*casovac)?"
_TIMER = r"\b(?:timer|countdown|casovac\w*)\b"
_REMIND = r"\b(?:remind\w*|pripomienk\w*|pripomen|pripomenuj|odpocitavaj\w*)\b"
_CANCEL = r"\b(?:cancel|stop(?:ping)?|zrus(?:it|te|ila)?|rus(?:it|te)?|vypni|vypnite|zastav(?:te)?|vymaz(?:te)?)\b"
_STATUS = r"\b(?:kolko|kolik|how\s+much|how\s+long|remaining|zostava|ostava|ostava|zbiva|left)\b"
_SETVERB = r"\b(?:set\s+(?:up\s+)?(?:a\s+)?(?:the\s+)?|start|startuj|spust(?:i|te|)|nastav(?:te|)|zapni|zapnite|establish|pripomienk\w*|remind\w*)?"

_PAIR_RE = re.compile(r"\b(" + _NUM + r")\b")
_TIMER_RE = re.compile(_TIMER)
_REMIND_RE = re.compile(_REMIND)
_CANCEL_RE = re.compile(_CANCEL)
_STATUS_RE = re.compile(_STATUS)

_UNIT_SECONDS = [
    (re.compile(r"(?:sec|sek)\w*$"), 1),
    (re.compile(r"^(?:mins?|minu\w*|m\w*)$"), 60),
    (re.compile(r"^(?:h\w*)$"), 3600),
]


def _fold(text: str) -> str:
    t = unicodedata.normalize("NFD", text.lower())
    t = "".join(c for c in t if not unicodedata.combining(c))
    return re.sub(r"\s+", " ", t).strip()


def _unit_seconds(unit: str) -> int:
    for rx, secs in _UNIT_SECONDS:
        if rx.search(unit):
            return secs
    return 60


def _num(token: str) -> int | None:
    token = token.split()[0]
    if token.isdigit():
        return int(token)
    return _NUMWORDS.get(token)


def parse_timer(text: str) -> tuple[str, int] | None:
    """Return ('set', seconds) | ('cancel', 0) | ('status', 0) | None."""
    t = _fold(text)
    if not t:
        return None
    # cancel: verb + timer word, no number required
    if _CANCEL_RE.search(t) and _TIMER_RE.search(t):
        return ("cancel", 0)
    # status: quantity word + timer word ("kolko ostava na casovaci")
    if _STATUS_RE.search(t) and _TIMER_RE.search(t):
        return ("status", 0)
    # set: a number+unit AND (timer word OR remind word)
    m = _PAIR_RE.search(t)
    if m and (_TIMER_RE.search(t) or _REMIND_RE.search(t)):
        raw = m.group(1)
        n = _num(raw)
        unit = raw[len(raw.split()[0]):].strip()
        seconds = (n or 0) * _unit_seconds(unit)
        if 0 < seconds <= 24 * 3600:
            return ("set", seconds)
    return None


def arm(device_id: int, seconds: int) -> None:
    _ACTIVE[device_id] = time.monotonic() + seconds


def cancel(device_id: int) -> bool:
    return _ACTIVE.pop(device_id, None) is not None


def remaining(device_id: int) -> int:
    end = _ACTIVE.get(device_id, 0.0)
    rem = int(round(end - time.monotonic()))
    if rem <= 0:
        _ACTIVE.pop(device_id, None)
        return 0
    return rem


def _pl_sk(n: int, one: str, few: str, many: str) -> str:
    if n == 1:
        return one
    if 2 <= n <= 4:
        return few
    return many


def fmt_sk(s: int) -> str:
    h, r = divmod(s, 3600)
    m, sec = divmod(r, 60)
    parts = []
    if h:
        parts.append(f"{h} {_pl_sk(h, 'hodinu', 'hodiny', 'hodín')}")
    if m:
        parts.append(f"{m} {_pl_sk(m, 'minútu', 'minúty', 'minút')}")
    if sec and not h:
        parts.append(f"{sec} {_pl_sk(sec, 'sekundu', 'sekundy', 'sekúnd')}")
    return " a ".join(parts) if parts else "0 sekúnd"


def fmt_en(s: int) -> str:
    h, r = divmod(s, 3600)
    m, sec = divmod(r, 60)
    parts = []
    if h:
        parts.append(f"{h} hour{'s' if h != 1 else ''}")
    if m:
        parts.append(f"{m} minute{'s' if m != 1 else ''}")
    if sec and not h:
        parts.append(f"{sec} second{'s' if sec != 1 else ''}")
    return " and ".join(parts) if parts else "0 seconds"
