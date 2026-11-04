# The estate card document

Agreed with demesne (session-e85c033f). Ward reads one small typed document to
answer "who holds the card?" and "may a judgement run now?".

## Document

```
GET /estate/card[?group=gpu|aux]   (gpu is demesne's default)
{"group":"gpu","holders":["sd"],"since_ms":1793711314557,
 "slots":[{"name":"llama","state":"stopped"},
          {"name":"sd","state":"running"}]}
```

Read it exactly as served:

- **`holders` is an array** and can be longer than one: during a handover both
  engines are up for the length of an eviction. Empty means idle. Do not assume
  length one and do not pick a "primary".
- **`since_ms` is when the card became busy**, not when the request was made, and
  **0 means unknown** (an engine demesne did not start; `llama` is no longer in that class — demesne starts it, so its holder reports a real time).
  Treat 0 as unknown, never as 1970.
- **`state` is `running | loading | stopped`**, and `loading` is "about to hold
  the card" — a cold `sd` spends minutes there. Treating `loading` as free is how
  a render gets evicted a minute later.
- **No `device` field**, deliberately: demesne models exclusivity groups, not
  driver strings, and a CUDA0 that names the wrong card is worse than none.

## Which card matters to ward

`gpu` is the 20 GiB RX 7900 XT — llama's `ROCm0`, sd's `vulkan0`. `aux` is the
2 GiB GT 1030, where **llamb lives and where ward's judge runs**.

So today **sd rendering does not contend with a judgement**: they are on
different cards, and refusing a judgement while `sd` holds `gpu` protects
nothing. The document is still the right thing to have built — it is what makes
the question answerable the day the judge moves to the big card — but the policy
it feeds is "no gate yet, and say why", not "refuse when gpu is busy".

## Rule ward must keep

The socket is for **control**, never for inference. An inference call through
demesne's default route would evict the media child before proxying, so routing a
judgement through it would kill a running render. `wd_http_set_unix_socket` exists
for `/estate/*` and nothing else.

## Status

The transport and the reader are in, and `estate_test` gates the document over a fixture socket. It also has an opt-in live
mode, `WARD_ESTATE_LIVE=gpu ./out/estate_test`, which reads the real socket with ward's own reader rather than curl.

Measured that way on 2026-11-04 against the deployed demesne:

    live estate: group=gpu holders=1 since_ms=1793721683083 slots=2

`holders` and `since_ms` now carry the real state; the earlier `holders: []` with llama loaded was the stricter holder rule
working, not a reader fault. A demesne restart does not move the socket: it stays at
`/run/user/1000/demesne.sock`, so live mode keeps pointing at the same path.

The read is bounded. The plain GET sets `CURLOPT_CONNECTTIMEOUT_MS` and `CURLOPT_TIMEOUT_MS` (5 s) on the same handle as
`CURLOPT_UNIX_SOCKET_PATH`, and `wd_http_job_finish` polls its pipe against the job's own timeout instead of blocking.
`/estate/card` never takes demesne's transition lock and answers in milliseconds even in the middle of an eight-minute sd
cold load, so 5 s is roughly fifteen times the worst legitimate case: if it fires, the socket is dead rather than busy.

The test cannot outlive its harness. `main` and the forked fixture server both arm `PR_SET_PDEATHSIG(SIGKILL)` — fork
clears the setting, so the child re-arms it — and the fixture's `accept` waits in `poll` with a 10 s deadline. The case
that once left two reparented processes is:

    timeout 1 env WARD_ESTATE_LIVE=gpu ./out/estate_test   # exit 124
    ps -eo pid,ppid,comm | grep estate_test                # empty

A listener that accepts and never answers returns the read in about 5 s rather than hanging; that is the path the
orphans sat behind. Treat a 10 s accept expiry as a slow machine to investigate, not a number to raise.
