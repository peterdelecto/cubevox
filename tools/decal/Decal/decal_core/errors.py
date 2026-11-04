"""Errors raised by decal_core."""


class DecalError(Exception):
    """A failure the user can act on.

    str(error) is the complete message shown in the dialog's message box.
    """
