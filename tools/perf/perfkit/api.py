"""A small JSON client for the stand's rfmap-server, and the set-up it needs before k6 starts.

The stand listens on 127.0.0.1 only, so requests never go through a proxy.
"""
import json
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any

from perfkit.errors import PerfError

_TIMEOUT_S = 30


@dataclass(frozen=True)
class Reply:
    status: int
    body: Any


class ApiClient:
    def __init__(self, base_url: str, opener: urllib.request.OpenerDirector | None = None):
        if not base_url.startswith(("http://127.0.0.1", "http://localhost")):
            raise PerfError(f"the stand must be local, not {base_url}")
        self.base_url = base_url.rstrip("/")
        self._opener = opener or urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def call(self, method: str, path: str, body: Any = None, headers: dict[str, str] | None = None) -> Reply:
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(f"{self.base_url}{path}", data=data, method=method,
                                         headers={"Content-Type": "application/json", **(headers or {})})
        try:
            with self._opener.open(request, timeout=_TIMEOUT_S) as response:
                return Reply(response.status, self._parse(response.read()))
        except urllib.error.HTTPError as error:
            return Reply(error.code, self._parse(error.read()))
        except (urllib.error.URLError, OSError) as error:
            raise PerfError(f"{method} {path}: {error}") from error

    def expect(self, status: int, method: str, path: str, body: Any = None,
               headers: dict[str, str] | None = None) -> Any:
        reply = self.call(method, path, body, headers)
        if reply.status != status:
            raise PerfError(f"{method} {path} answered {reply.status}, expected {status}: {reply.body}")
        return reply.body

    @staticmethod
    def _parse(raw: bytes) -> Any:
        if not raw:
            return None
        try:
            return json.loads(raw)
        except ValueError:
            return raw.decode("utf-8", errors="replace")


class Provisioner:
    """Creates what the personas sign in with: missions, their owner keys and share links."""

    # Six digits, as the server demands; the stand lives for one run.
    PIN = "246810"

    def __init__(self, client: ApiClient):
        self._client = client

    def credentials(self, auth: dict, missions: list[dict], count: int) -> list[dict]:
        kind = auth["type"]
        if kind in ("none", "admin-token"):
            return []
        if not missions:
            raise PerfError(f"{kind} needs setup.missions to sign in with")
        found = []
        for index in range(count):
            created = self._client.expect(201, "POST", "/missions", missions[index % len(missions)])
            credential = {"missionId": created["missionId"], "ownerKey": created["ownerKey"],
                          "session": created["session"]}
            if kind in ("link-pin", "link-token"):
                credential.update(self._link(credential, auth.get("role", "edit")))
            found.append(credential)
        return found

    def _link(self, credential: dict, role: str) -> dict:
        link = self._client.expect(201, "POST", f"/missions/{credential['missionId']}/links",
                                   {"role": role, "pin": self.PIN, "label": "perf"},
                                   {"X-Owner-Key": credential["ownerKey"]})
        return {"token": link["token"], "pin": self.PIN}
