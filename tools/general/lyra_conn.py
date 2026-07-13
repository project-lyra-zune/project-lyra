"""A reused daemon connection for the tree walker and snapshot puller.

Reconnect-on-error is the resync mechanism: an error mid-command can leave
undrained bytes on the socket, so a failing caller drops the connection."""
from lyra_rdfile import connect
from lyra_lsdir import list_dir


class DeviceConn:
    def __init__(self, ip, port, timeout):
        self.ip = ip
        self.port = port
        self.timeout = timeout
        self._sock = None

    def sock(self):
        if self._sock is None:
            self._sock = connect(self.ip, self.port, self.timeout)
        return self._sock

    def drop(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None


def lsdir(conn, path, timeout):
    """List a directory; drop the connection on error so a desync can't reach the next command."""
    try:
        return list(list_dir(conn.sock(), path, timeout))
    except Exception:
        conn.drop()
        raise
