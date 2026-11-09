#!/usr/bin/env python3
# Serves this directory on 127.0.0.1:8123 and waits for the page's POST to
# /result. Exit 0 if every frame came out, 1 otherwise.
import http.server
import json
import sys

OK = False


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_POST(self):
        global OK
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.send_response(204)
        self.end_headers()
        result = json.loads(body)
        print("RESULT", json.dumps(result), flush=True)
        OK = (
            result.get("frames") == 10
            and set(result.get("sizes", [])) == {"320x240"}
            and not result.get("errors")
        )
        self.server.done = True

    def log_message(self, *args):
        pass


server = http.server.HTTPServer(("127.0.0.1", 8123), Handler)
server.done = False
server.timeout = 1
for _ in range(int(sys.argv[1]) if len(sys.argv) > 1 else 90):
    server.handle_request()
    if server.done:
        break
else:
    print("TIMEOUT: no result posted", flush=True)
print("PASS" if OK else "FAIL", flush=True)
sys.exit(0 if OK else 1)
