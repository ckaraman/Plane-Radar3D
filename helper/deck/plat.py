"""The parts that differ between Windows and macOS, picked once."""

import sys

IS_MAC = sys.platform == "darwin"

if IS_MAC:
    from . import actions_mac as actions  # noqa: F401
    from . import media_mac as media  # noqa: F401
    from . import programs_mac as programs  # noqa: F401
    from . import volume_mac as volume  # noqa: F401
else:
    from . import actions  # noqa: F401
    from . import media  # noqa: F401
    from . import programs  # noqa: F401
    from . import volume  # noqa: F401
