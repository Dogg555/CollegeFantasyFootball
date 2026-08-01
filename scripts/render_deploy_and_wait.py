#!/usr/bin/env python3
"""Trigger a specific Render deploy, wait for it to become live, then verify health."""

import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


API_ROOT = "https://api.render.com/v1"
API_KEY = os.environ.get("RENDER_API_KEY", "").strip()
SERVICE_ID = os.environ.get("RENDER_SERVICE_ID", "").strip()
COMMIT_SHA = os.environ.get("RENDER_COMMIT_SHA", "").strip()
APP_URL = os.environ.get("CFF_API_BASE_URL", "").strip().rstrip("/")
POLL_SECONDS = int(os.environ.get("RENDER_POLL_SECONDS", "15"))
TIMEOUT_SECONDS = int(os.environ.get("RENDER_DEPLOY_TIMEOUT_SECONDS", "2700"))


class DeployFailure(RuntimeError):
    pass


def require(value, name):
    if not value:
        raise DeployFailure(f"Missing required environment variable: {name}")


def render_request(method, path, payload=None):
    data = None
    headers = {
        "Accept": "application/json",
        "Authorization": f"Bearer {API_KEY}",
        "Content-Type": "application/json",
    }
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(f"{API_ROOT}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            body = response.read().decode("utf-8")
            return response.getcode(), json.loads(body) if body else {}
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8")
        raise DeployFailure(f"Render API {method} {path} returned {exc.code}: {body}") from exc
    except urllib.error.URLError as exc:
        raise DeployFailure(f"Render API {method} {path} failed: {exc}") from exc


def deploy_object(payload):
    if isinstance(payload, dict) and isinstance(payload.get("deploy"), dict):
        return payload["deploy"]
    return payload if isinstance(payload, dict) else {}


def find_queued_deploy():
    query = urllib.parse.urlencode({"limit": 20})
    _, payload = render_request("GET", f"/services/{SERVICE_ID}/deploys?{query}")
    items = payload if isinstance(payload, list) else payload.get("deploys", [])
    for item in items:
        deploy = deploy_object(item)
        commit = deploy.get("commit") or {}
        deployed_sha = deploy.get("commitId") or commit.get("id") or commit.get("sha")
        if not COMMIT_SHA or (deployed_sha and COMMIT_SHA.startswith(str(deployed_sha))) or (
            deployed_sha and str(deployed_sha).startswith(COMMIT_SHA)
        ):
            if deploy.get("id"):
                return deploy
    raise DeployFailure("Render queued the deploy but no matching deploy ID could be found")


def trigger_deploy():
    payload = {"clearCache": "do_not_clear"}
    if COMMIT_SHA:
        payload["commitId"] = COMMIT_SHA
    status_code, response = render_request("POST", f"/services/{SERVICE_ID}/deploys", payload)
    deploy = deploy_object(response)
    if not deploy.get("id"):
        if status_code == 202:
            time.sleep(POLL_SECONDS)
            deploy = find_queued_deploy()
        else:
            raise DeployFailure(f"Render did not return a deploy ID: {response}")
    return deploy


def wait_for_deploy(deploy_id):
    deadline = time.monotonic() + TIMEOUT_SECONDS
    last_status = None
    failure_states = {
        "build_failed",
        "update_failed",
        "canceled",
        "cancelled",
        "deactivated",
        "failed",
    }
    while time.monotonic() < deadline:
        _, response = render_request("GET", f"/services/{SERVICE_ID}/deploys/{deploy_id}")
        deploy = deploy_object(response)
        status = str(deploy.get("status", "unknown")).lower()
        if status != last_status:
            print(json.dumps({"deployId": deploy_id, "status": status, "commit": deploy.get("commit")}, default=str))
            last_status = status
        if status == "live":
            return deploy
        if status in failure_states or status.endswith("_failed"):
            raise DeployFailure(f"Render deploy {deploy_id} ended with status {status}: {deploy}")
        time.sleep(POLL_SECONDS)
    raise DeployFailure(f"Timed out waiting for Render deploy {deploy_id}; last status was {last_status}")


def wait_for_health():
    deadline = time.monotonic() + 600
    last_error = "not attempted"
    while time.monotonic() < deadline:
        try:
            request = urllib.request.Request(f"{APP_URL}/health", headers={"Accept": "application/json"})
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = json.loads(response.read().decode("utf-8"))
            if payload.get("status") == "ok" and payload.get("database") == "ok":
                return payload
            last_error = f"unexpected payload: {payload}"
        except (urllib.error.URLError, urllib.error.HTTPError, json.JSONDecodeError) as exc:
            last_error = str(exc)
        time.sleep(10)
    raise DeployFailure(f"Render deploy became live, but health never reported database=ok: {last_error}")


def write_output(name, value):
    output_path = os.environ.get("GITHUB_OUTPUT")
    if output_path:
        with open(output_path, "a", encoding="utf-8") as handle:
            handle.write(f"{name}={value}\n")


def main():
    require(API_KEY, "RENDER_API_KEY")
    require(SERVICE_ID, "RENDER_SERVICE_ID")
    require(APP_URL, "CFF_API_BASE_URL")

    deploy = trigger_deploy()
    deploy_id = deploy["id"]
    print(f"Triggered Render deploy {deploy_id} for commit {COMMIT_SHA or 'latest'}")
    completed = wait_for_deploy(deploy_id)
    health = wait_for_health()
    write_output("deploy_id", deploy_id)
    write_output("deploy_status", completed.get("status", "live"))
    print(json.dumps({"status": "ok", "deploy": completed, "health": health}, indent=2, default=str))


if __name__ == "__main__":
    try:
        main()
    except DeployFailure as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
