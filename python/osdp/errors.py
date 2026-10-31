#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Exceptions raised by the osdp package."""

from .enums import CompletionStatus, NakCode, TrsFailReason

__all__ = ["MarshalError", "NakError", "OSDPError", "TrsSessionError"]


class OSDPError(Exception):
    """Base class for every error this package raises."""


class NakError(OSDPError):
    """Raised by a PD command handler to reject a command.

    The library turns this into a NAK reply to the CP. Raising it is the only
    way for a handler to decline a command; returning normally acknowledges it.

    @see osdp_pd_nak_code_e

    Example:
        >>> raise NakError(NakCode.BioType)
        Traceback (most recent call last):
        osdp.errors.NakError: Command rejected: BioType
    """

    def __init__(self, code: NakCode = NakCode.Record) -> None:
        self.code = code
        """Why the command was rejected."""

        super().__init__(f"Command rejected: {code.name}")


class TrsSessionError(OSDPError):
    """Raised by a `ControlPanel.trs_session()` when a card session cannot
    open, or a card exchange in it did not happen.

    A card that answered with an error status word has still answered: that
    is a successful exchange, and nothing is raised.

    Example:
        >>> raise TrsSessionError("card exchange failed",
        ...                       CompletionStatus.Failed, code=0x01)
        Traceback (most recent call last):
        osdp.errors.TrsSessionError: card exchange failed (Failed, reader error 0x01)
    """

    def __init__(
        self,
        message: str,
        status: CompletionStatus | None = None,
        code: int | None = None,
        reason: TrsFailReason | None = None,
    ) -> None:
        self.status = status
        """How the command settled: Failed when the reader reported an error
        or never answered, Flushed when the session ended under it. None when
        the session could not be opened at all."""

        self.code = code
        """The reader's error code, when it sent one. Reader-defined: OSDP
        does not standardize it."""

        self.reason = reason
        """Why the session failed, when it did: the command failed or was
        flushed because the session ended. None while the session is up."""

        detail = [status.name] if status is not None else []
        if code is not None:
            detail.append(f"reader error 0x{code:02x}")
        if reason is not None:
            detail.append(f"session {reason.name}")
        if detail:
            message = f"{message} ({', '.join(detail)})"
        super().__init__(message)


class MarshalError(OSDPError):
    """Raised when a command or event cannot cross the C boundary.

    This means a malformed object was submitted, or the library produced
    something this version of the bindings does not understand. Either way it
    is a bug rather than a runtime condition to be handled.
    """
