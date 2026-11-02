# A golden transcript

Captured with `WARD_DUMP_FRAMES=1 ./out/ward-native create /tmp`, which prints
every frame the client sends and receives as hex. A second consumer — the
Android client in particular — can diff its own encoding against these bytes
instead of guessing at the format.

- `frame type=2 sub=0 seq=0 len=101`

```
a4 61 76 1b 00 00 00 00 00 00 00 01 66 73 65 72 76 65 72 6a 77 61 72 64 2f 30 2e 30 2e 31 64 61 75 74 68 a2 63 75 69 64 1b 00 00 00 00 00 00 03 e8 65 73 63 6f 70 65 65 6c 6f 63 61 6c 66 6c 69 6d 69 74 73 a2 64 62 6f 64 79 1b 00 00 00 00 00 01 00 00 68 69 6e 66 6c 69 67 68 74 1b 00 00 00 00 00 00 00 40
```
- **sent session.create**
- **sent frame len=52 body=a36269641b0000000000000001666d6574686f646e73657373696f6e2e63726561746566706172616d73a163637764642f746d70**
- `frame type=4 sub=0 seq=0 len=154`

```
a3 62 69 64 1b 00 00 00 00 00 00 00 01 62 6f 6b f5 66 72 65 73 75 6c 74 a1 67 73 65 73 73 69 6f 6e a9 62 69 64 70 66 30 35 36 39 64 32 35 65 34 38 33 35 66 66 30 63 63 77 64 64 2f 74 6d 70 65 6d 6f 64 65 6c 65 75 6e 73 65 74 67 63 72 65 61 74 65 64 1b 00 00 01 a1 9c 1e fc de 68 6c 61 73 74 5f 73 65 71 1b 00 00 00 00 00 00 00 01 64 6e 61 6d 65 60 68 61 72 63 68 69 76 65 64 f4 66 70 69 6e 6e 65 64 f4 6a 75 70 64 61 74 65 64 5f 6d 73 1b 00 00 01 a1 9c 1e fc fd
```

The first frame is the daemon's `welcome` in reply to `hello`; the second is
the `session.create` request; the third is its response. Types are the frame's
`type` field, 2 for a control frame and 4 for a response.

Everything on the wire here uses definite maps and arrays, text, unsigned
integers and booleans — no tags and no indefinite-length forms — which is the
subset a hand-written encoder needs.
