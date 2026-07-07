import abc

class Channel(abc.ABC):
    def __init__(self) -> None:
        pass

    @abc.abstractmethod
    def read(self, max_bytes: int) -> bytes:
        """
        Read at most `max_bytes` bytes from the stream and return the data as
        a byte array.
        """
        return []

    @abc.abstractmethod
    def write(self, buf: bytes) -> int:
        """
        Send buf (as much as possible) over the raw stream and return the
        number of bytes that were actually sent. This has be to <= len(buf).
        """
        return len(buf)

    @abc.abstractmethod
    def flush(self) -> None:
        """
        If the underlying stream supports it, perform a flush here. If not,
        can return without doing anything.
        """
        pass
