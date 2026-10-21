// The one k6 script of the perf toolchain. perf.py writes plan.json (the load profile, and per
// persona its share of the load, credentials, steps and request bodies); this script turns every
// persona into a k6 scenario of its own and runs one weighted step per iteration.
//
// Every request is tagged with persona and step, so latency, rate and errors can be read per
// step. A tiny always-true threshold per tag keeps those sub-metrics in the summary.

import http from "k6/http";
import exec from "k6/execution";
import { check, sleep } from "k6";
import { Trend } from "k6/metrics";

const PLAN_PATH = __ENV.PLAN || "/run/perf/plan.json";
const SUMMARY_PATH = __ENV.SUMMARY || "/run/perf/k6-summary.json";
const plan = JSON.parse(open(PLAN_PATH));
const jobDuration = new Trend("job_duration", true);

// Arrival rates are set per minute, so a persona with a small share still gets whole iterations.
const perMinute = (rps, share) => Math.max(0, Math.round(rps * share * 60));

function executorFor(persona) {
  const vus = plan.vus || { preAllocated: 5, max: 200 };
  const base = {
    exec: "step",
    env: { PERSONA: persona.id },
    tags: { persona: persona.id },
    timeUnit: "1m",
    preAllocatedVUs: vus.preAllocated,
    maxVUs: vus.max,
    gracefulStop: "30s",
  };
  const stages = plan.load.stages;
  if (plan.load.type === "constant-arrival-rate") {
    return { ...base, executor: "constant-arrival-rate", rate: Math.max(1, perMinute(stages[0].targetRps, persona.share)),
             duration: `${stages[0].durationS}s` };
  }
  return { ...base, executor: "ramping-arrival-rate", startRate: 0,
           stages: stages.map((s) => ({ duration: `${s.durationS}s`, target: perMinute(s.targetRps, persona.share) })) };
}

function thresholds() {
  const found = { ...(plan.thresholds || {}) };
  const keep = (name) => { if (!(name in found)) found[name] = ["count>=0"]; };
  const keepTrend = (name) => { if (!(name in found)) found[name] = ["avg>=0"]; };
  for (const persona of plan.personas) {
    for (const step of persona.steps) {
      const tags = `{persona:${persona.id},step:${step.name}}`;
      keepTrend(`http_req_duration${tags}`);
      keep(`http_reqs${tags}`);
      found[`http_req_failed${tags}`] = found[`http_req_failed${tags}`] || ["rate>=0"];
      if (step.poll) keepTrend(`job_duration${tags}`);
    }
  }
  return found;
}

export const options = {
  scenarios: Object.fromEntries(plan.personas.map((p) => [p.id, executorFor(p)])),
  thresholds: thresholds(),
  summaryTrendStats: ["avg", "min", "med", "max", "p(90)", "p(95)", "p(99)"],
  discardResponseBodies: false,
  // No url and name tags: paths hold mission ids, and Prometheus would get a series per mission.
  // persona and step name every request instead.
  systemTags: ["status", "method", "scenario", "expected_response", "error_code", "check"],
};

// Per persona: the step chooser (cumulative weights) and its credentials.
const personas = Object.fromEntries(plan.personas.map((p) => {
  let total = 0;
  const steps = p.steps.map((s) => { total += s.weight; return { ...s, upTo: total }; });
  return [p.id, { ...p, steps, total }];
}));

// Sessions this VU opened, by persona; a VU signs in once, like a person keeps a tab open.
const sessions = {};

function credentialOf(persona) {
  const list = persona.credentials || [];
  return list.length ? list[(exec.vu.idInTest - 1) % list.length] : {};
}

function authHeaders(persona, credential) {
  const auth = persona.auth.type;
  if (auth === "admin-token") return { Authorization: `Bearer ${plan.adminToken}` };
  if (auth === "mission-owner") return { Authorization: `Bearer ${credential.session}` };
  if (auth !== "link-pin") return {};
  if (!sessions[persona.id]) {
    const res = http.post(`${plan.baseUrl}/links/${credential.token}/open`, JSON.stringify({ pin: credential.pin }), {
      headers: { "Content-Type": "application/json" }, tags: { persona: persona.id, step: "open-link" } });
    check(res, { "open-link answers 200": (r) => r.status === 200 }, { persona: persona.id, step: "open-link" });
    if (res.status !== 200) return null;
    sessions[persona.id] = res.json("session");
  }
  return { Authorization: `Bearer ${sessions[persona.id]}` };
}

function pick(persona) {
  const at = Math.random() * persona.total;
  return persona.steps.find((s) => at < s.upTo) || persona.steps[persona.steps.length - 1];
}

// A step's bodies are sent in turn across all VUs of the persona.
function bodyOf(step) {
  if (!step.bodies || step.bodies.length === 0) return null;
  return step.bodies[exec.scenario.iterationInTest % step.bodies.length];
}

function fill(path, credential, response) {
  return path.replace(/\{\{([A-Za-z]+)(?:\.([A-Za-z]+))?\}\}/g, (_, name, field) => {
    if (name === "response") return encodeURIComponent(String(response?.[field] ?? ""));
    return encodeURIComponent(String(credential[name] ?? ""));
  });
}

function poll(step, credential, headers, first, tags) {
  const started = Date.now();
  const pollTags = { ...tags, step: `${step.name}-poll` };
  let body = first;
  while (step.poll.pending.includes(String(body?.[step.poll.untilField]))) {
    if (Date.now() - started > step.poll.timeoutS * 1000) {
      check(null, { [`${step.name} finishes in ${step.poll.timeoutS} s`]: () => false }, tags);
      return;
    }
    sleep(step.poll.intervalS || 1);
    const res = http.get(`${plan.baseUrl}${fill(step.poll.path, credential, first)}`, { headers, tags: pollTags });
    if (res.status !== 200) return;
    body = res.json();
  }
  jobDuration.add(Date.now() - started, tags);
  check(body, { [`${step.name} ends well`]: (b) => !["failed", "error"].includes(String(b?.[step.poll.untilField])) }, tags);
}

export function setup() {
  return { startedMs: Date.now() };
}

export function step() {
  const persona = personas[__ENV.PERSONA];
  const credential = credentialOf(persona);
  const auth = authHeaders(persona, credential);
  if (auth === null) return;
  const s = pick(persona);
  const tags = { persona: persona.id, step: s.name };
  const body = bodyOf(s);
  const headers = { ...auth, "Content-Type": "application/json" };
  const res = http.request(s.method, `${plan.baseUrl}${fill(s.path, credential, null)}`,
                           body === null ? null : JSON.stringify(body), { headers, tags, timeout: "120s" });
  const expected = s.expect || 200;
  check(res, { [`${s.name} answers ${expected}`]: (r) => r.status === expected }, tags);
  if (s.poll && res.status === expected) poll(s, credential, auth, res.json(), tags);
  if (persona.thinkTimeS) sleep(persona.thinkTimeS.min + Math.random() * (persona.thinkTimeS.max - persona.thinkTimeS.min));
}

export function handleSummary(data) {
  const reqs = data.metrics.http_reqs ? data.metrics.http_reqs.values.count : 0;
  return {
    [SUMMARY_PATH]: JSON.stringify(data),
    stdout: `k6: ${reqs} request(s); summary in ${SUMMARY_PATH}\n`,
  };
}
