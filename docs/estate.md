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
  **0 means unknown** (an engine demesne did not start, e.g. `llama` today).
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

The transport is in (`5aa0299`); the capability is not written. Nothing can be
tested live until demesne 0.3.0 is deployed at `$XDG_RUNTIME_DIR/demesne.sock` —
what owns `:55558` today is stadi 0.2.0, which has no `-socket`. The shape here is
committed and stable, so the capability can be written against it and left
labelled unmeasured until the socket answers.
