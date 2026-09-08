# Initial SDK review and corrections

Date: 2026-09-08. Review scope: `5c583a916a9fb5ee4da5394757292307d8de5e60`
through `8a3218cd5c9465f06df371f040a3641188bd4900` (initial implementation
`c854cbaf51a6e12603d90e694697a43461744c5f` and Windows CI baseline fix).
Two independent reviewers checked documented standards and the original request
against WuKongEasySDK-JS `9c03c98c725982fac224cd1d3b52456eae983975`.
No linked issue or separate feature specification existed; the user request,
reference implementation, and documented C++ adaptation define this scope.

## Standards

- **P1, corrected:** listener capture destructors ran under `listenerMutex` in
  `off()` and terminal cleanup. A capture that removed another subscription
  deadlocked. Both paths were reproduced with a three-second subprocess deadline.
  Nodes/maps now leave the locked container before captures are destroyed;
  unit regressions cover both paths.
- **P2, corrected:** malformed SENDACK rejected only its send future, contrary
  to the README contract requiring malformed protocol to close the session.
  A successful reason without message identifiers reproduced the missing
  Disconnect. Protocol parse failures now terminate without reconnect;
  valid server rejection still fails only the send.
- **P2, corrected:** oversize and invalid WebSocket frames were classified as
  retryable network errors. Both reproduced unwanted Reconnecting with default
  automatic reconnect enabled. Transport now classifies protocol/size violations
  as terminal; real-socket regressions retain automatic reconnect.
- **P2, corrected:** callback-owned destruction validation released stack
  captures before queued terminal cleanup was guaranteed to finish. The test
  now hands a shutdown future to the main thread and waits before releasing
  captured state. This was a source-proven race, not a reproduced sanitizer crash.

No additional baseline design-smell finding was reported.

## Spec

No missing executable JS API requirement, repository naming mismatch, or
unsupported feature claim was confirmed. The C++ independent-client, futures,
listener-token, default-device, bounds, and error-sanitization adaptations are
explicitly documented. The callback-destruction validation race above was also
reported on this axis and is corrected by the same shutdown barrier.

Standards: four corrected findings (worst P1); Spec: one corrected validation
finding (P2, shared with Standards), no confirmed missing product functionality.
See [validation evidence](VALIDATION.md) for executed checks and their limits.
