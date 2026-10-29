"""A fake NameNode that answers every WebHDFS request the way a standby NameNode does (HTTP 403 with a
StandbyException), for the failover tests. Usage: python3 standby_namenode.py <port>"""

import http.server
import json
import sys

BODY = json.dumps(
    {
        "RemoteException": {
            "exception": "StandbyException",
            "javaClassName": "org.apache.hadoop.ipc.StandbyException",
            "message": "Operation category READ is not supported in state standby",
        }
    }
).encode()


class StandbyHandler(http.server.BaseHTTPRequestHandler):
    def respond(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        self.send_response(403)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)

    do_GET = do_PUT = do_POST = do_DELETE = respond

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), StandbyHandler).serve_forever()
