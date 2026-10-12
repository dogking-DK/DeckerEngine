"""Reconcile durable batch evidence before continuing or explicitly retrying work."""

import os

from ._experiment_lock import exclusive_path
from .experiment_batch import (_Runner, _RetryRequired, _Unconfirmed, _capabilities, _directory,
                               _item, _options, _runtime_lease, read_progress)
from .experiment_index import (_errors, _inside, _inspect, _root, environment,
                               register_artifacts, verify_index)
from .experiments import canonical, clone_json, require
from .models import Ticket


class _RecoveryRunner(_Runner):
    retry_failed = False

    def bind_artifacts(self, entry):
        """Adopt a complete published directory or existing index commit, never overwrite it."""
        item = self.current
        if not os.path.lexists(self.root / item["directory"]):
            require(item["artifact_attempt_id"] is None, "Registered artifacts are missing")
            return False
        evidence = _inspect(self.root, entry, item["directory"], item["run_id"])
        require(all(evidence["source"][k] == self.report["source"][k] for k in ("document_id", "scene_id", "revision")),
                "Recovered artifact scene source mismatch")
        index = verify_index(self.root)
        indexed = next(e for e in index["experiments"] if e["experiment_id"] == entry["experiment_id"])
        attempts = [a for a in indexed["attempts"] if a["directory"] == item["directory"]]
        if attempts:
            attempt = attempts[0]
            require(attempt["source"]["run_id"] == item["run_id"], "Recovered artifact run mismatch")
        else:
            self.budget()
            attempt = register_artifacts(self.root, entry["experiment_id"], item["directory"],
                expected_run_id=item["run_id"], execution_environment=self.observed)
        item.update(artifact_attempt_id=attempt["attempt_id"], completed_steps=item["target_steps"],
                    submitted_steps=item["target_steps"], initialized=True, batch_active=False)
        self.save()
        return True

    def collect(self, entry, run):
        if not self.bind_artifacts(entry):
            super().collect(entry, run)

    def resolved_failure(self, status, message):
        self.current.update(status=status, error={"type": "RecoveredFailure", "message": message})
        self.save()

    def retry(self, entry, state):
        item = self.current
        if not self.retry_failed:
            raise _RetryRequired("Resolved failed/cancelled attempt retained; use --retry-failed explicitly")
        require(item["status"] in ("failed", "cancelled"), "Unknown work cannot be retried")
        require(len(item["history"]) < 127, "Attempt limit exceeded")
        destination = _directory(entry["experiment_id"], len(item["history"]) + 1)
        require(not os.path.lexists(self.root / destination), "Retry directory already exists")
        if state["run"] is not None:
            # Identity/config/source were checked before reaching this method.
            if state["mode"] == "stopping":
                self.wait_stopped(entry, state, allow_terminal=True)
            else:
                self.stop(entry, allow_terminal=True)
        old = clone_json({k: v for k, v in item.items() if k != "history"})
        replacement = _item(entry, [*item["history"], old])
        item.clear()
        item.update(replacement)
        self.save()  # One commit retains the old attempt and chooses the new directory.
        super().process(entry)

    def process(self, entry):
        item = self.current
        source = self.call("scene.query")["state"]
        require(canonical(source) == canonical(self.report["source"]), "Edit source changed during recovery")
        if item["status"] == "failed" and item["run_id"] is None and item["request"] and item["request"]["execution"] == "unknown":
            item["status"] = "unknown"  # A legacy driver failure is not proof that its submission never ran.
            self.save()
        if item["status"] == "pending":
            return super().process(entry)
        state = self.call("simulation.query")
        if item["run_id"] is None and item["status"] not in ("failed", "cancelled"):
            request = item["request"]
            if request is None or request["execution"] == "not_sent":
                self.resolved_failure("failed", "No simulation request was sent")
            elif request["ticket"] is None or request["ticket"]["session"] != self.report["session"]:
                raise _Unconfirmed("Run identity and original ticket unavailable; cannot resolve or retry safely")
            else:
                # Same ticket+content is at most once; a missing/expired ledger record never gets a new ticket.
                receipt = self.call("simulation.run", request["params"], retry=Ticket(**request["ticket"]))
                self.accept(receipt)
                state = self.call("simulation.query")
        if state["run"] is not None:
            if item["run_id"] != state["run"]["run_id"]:
                raise _Unconfirmed("A different or unidentified simulation is active; no control was sent")
            run = self.observe(state, entry, allow_terminal=True)
            if item["status"] in ("failed", "cancelled"):
                return self.retry(entry, state)
            task_status = run["task"]["status"]
            if task_status in ("failed", "cancelled") or run["fault"] is not None:
                self.resolved_failure("cancelled" if task_status == "cancelled" else "failed",
                                      f"Runtime attempt {task_status}: {run['fault']}")
                return self.retry(entry, state)
            if task_status == "stopping":
                # Stop may already have been accepted at the previous process's last boundary.
                self.wait_stopped(entry, state)
                state = {"mode": "edit", "run": None}
            else:
                return self.finish(entry, state)
        require(state == {"mode": "edit", "run": None}, "Unexpected simulation state during recovery")
        if item["status"] in ("failed", "cancelled"):
            return self.retry(entry, state)
        if item["run_id"] is not None and self.bind_artifacts(entry):
            item.update(status="succeeded", error=None)
            self.save()
            return
        self.resolved_failure("failed", "Accepted run no longer exists and no complete artifacts were published")
        self.retry(entry, state)


def resume(client, *, project_root, output, retry_failed, experiment_timeout, poll_interval,
           runner_executable, progress):
    _options(experiment_timeout, poll_interval, progress)
    require(type(retry_failed) is bool, "retry_failed must be boolean")
    require(os.name == "nt", "Batch recovery requires the Windows local pipe client")
    with _errors():
        project = _root(project_root)
        root = _root(_inside(project, output))
        require(len(output.encode("utf-8")) <= 800, "Output path is too long for simulation.export")
        with exclusive_path("batch", root):
            report = read_progress(root)
            index = verify_index(root)
            if report["status"] == "succeeded":
                return report  # Verified no-op, even if the original host has since exited.
            require(client.endpoint == report["endpoint"], "Recovery requires the original endpoint")
            session = client.hello()["session"]
            require(session == report["session"], "Runtime session changed; old work cannot be rebound or retried")
            with _runtime_lease(session):
                def preflight(method):
                    reply = client.call(method)
                    require(reply.ticket.session == session, "Runtime changed during recovery preflight")
                    return reply.value
                source = preflight("scene.query")["state"]
                require(canonical(source) == canonical(report["source"]), "Recovery scene source changed")
                caps = preflight("runtime.capabilities")
                _capabilities(caps, [e for e, item in zip(index["experiments"], report["experiments"])
                                     if item["status"] != "succeeded"])
                observed = environment(executable=runner_executable, runtime=caps)
                runner = _RecoveryRunner(client, index, root, output, source, session, observed,
                                         experiment_timeout, poll_interval, progress)
                if report["version"] == 1:
                    report["version"] = 2
                    for item in report["experiments"]:
                        request = item["submission"] or item["last_call"]
                        item["request"] = clone_json(request) if request and request["method"] == "simulation.run" else None
                        item["history"] = []
                runner.report = report
                runner.retry_failed = retry_failed
                return runner.execute()
