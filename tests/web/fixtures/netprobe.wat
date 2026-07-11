;; §4.4 browser-backend probe (tests/web/net_test.mjs). The module never
;; needs page-side observability: everything it observes it reports back
;; through the network, and the test server's transcript is the
;; assertion. No tick input — after the first evaluation every update is
;; an external wake (HTTP completion, WS lifecycle/message).
;;
;; State machine, one hop per wake:
;;   0  render the port inputs into the URL table, then issue three GETs:
;;      /data (granted, CORS-permissive), /leak on the DENIED port
;;      (declared but ungranted: must report the offline face and never
;;      touch the wire), /nocors (granted origin, response without
;;      Access-Control-Allow-Origin: the browser must block it)
;;   1  once all three settle: POST the /data body to
;;      /report?d=<leak state>&c=<nocors state>, then open the WebSocket
;;   2  on the open wake: send "hello"
;;   3  on the message wake: echo it back as "e:<msg>"
;;   4  on the close wake: GET /close?w=<ws state>  (server closed: 3)
;;   5  done (idle)
;;
;; Token budget (burst 8, §4.4): 3 GETs + report + ws open + hello +
;; echo + close report = exactly 8. Do not add a request without
;; removing one.
;;
;; Ports arrive as value inputs ("denied" < "port", lexicographic io
;; order) and are rendered zero-padded to 5 digits, so URL bytes match
;; the interface's declared origins exactly (§4.4 byte-exact matching);
;; the URL parser treats leading zeros as the same port.
;;
;; Memory map:
;;     0 state    4 hData    8 hLeak   12 hNocors  16 hWs
;;   240 "hello"
;;   256 data URL (27, port@273)    304 nocors URL (29, port@321)
;;   352 report URL (37, port@369, d@384, c@388)
;;   400 close URL (32, port@417, w@431)
;;   440 ws URL (23, port@455)     480 leak URL (27, port@497)
;;  2048 http body buffer (cap 512)
;;  2600 "e:" + echo payload (cap 256)
;;  4096 io block: header 32 | in denied@+32 port@+36 | out state@+40
;;
;; Rebuild: tests/web/fixtures/build.sh (wasmtime's wat2wasm, the same
;; pinned C API the native tests embed).
(module
  (import "env" "drift_http_request"
    (func $req (param i32 i32 i32 i32 i32) (result i32)))
  (import "env" "drift_ws_open" (func $wsopen (param i32 i32) (result i32)))
  (import "env" "drift_net_poll" (func $poll (param i32 i32 i32) (result i32)))
  (import "env" "drift_net_send"
    (func $send (param i32 i32 i32 i32) (result i32)))
  (import "env" "drift_net_stat" (func $stat (param i32 i32) (result i32)))
  (memory (export "memory") 1)

  (data (i32.const 240) "hello")
  (data (i32.const 256) "http://127.0.0.1:00000/data")
  (data (i32.const 304) "http://127.0.0.1:00000/nocors")
  (data (i32.const 352) "http://127.0.0.1:00000/report?d=0&c=0")
  (data (i32.const 400) "http://127.0.0.1:00000/close?w=0")
  (data (i32.const 440) "ws://127.0.0.1:00000/ws")
  (data (i32.const 480) "http://127.0.0.1:00000/leak")
  (data (i32.const 2600) "e:")

  ;; 5 digits, zero-padded, at dst..dst+4.
  (func $wport (param $dst i32) (param $port i32)
    (local $i i32)
    (local.set $i (i32.const 4))
    (block $done
      (loop $l
        (i32.store8 (i32.add (local.get $dst) (local.get $i))
          (i32.add (i32.const 48)
                   (i32.rem_u (local.get $port) (i32.const 10))))
        (local.set $port (i32.div_u (local.get $port) (i32.const 10)))
        (br_if $done (i32.eqz (local.get $i)))
        (local.set $i (i32.sub (local.get $i) (i32.const 1)))
        (br $l))))

  (func (export "drift_abi") (result i32) (i32.const 1))
  (func (export "drift_init") (param i32) (result i32) (i32.const 4096))

  (func (export "drift_update")
    (local $s i32) (local $n i32) (local $a i32)
    (local.set $s (i32.load (i32.const 0)))

    (if (i32.eqz (local.get $s)) (then
      (local.set $a (i32.trunc_f32_u (f32.load (i32.const 4132)))) ;; port
      (call $wport (i32.const 273) (local.get $a))
      (call $wport (i32.const 321) (local.get $a))
      (call $wport (i32.const 369) (local.get $a))
      (call $wport (i32.const 417) (local.get $a))
      (call $wport (i32.const 455) (local.get $a))
      (call $wport (i32.const 497)
        (i32.trunc_f32_u (f32.load (i32.const 4128)))) ;; denied
      (i32.store (i32.const 4)
        (call $req (i32.const 256) (i32.const 27)
                   (i32.const 0) (i32.const 0) (i32.const 0)))
      (i32.store (i32.const 8)
        (call $req (i32.const 480) (i32.const 27)
                   (i32.const 0) (i32.const 0) (i32.const 0)))
      (i32.store (i32.const 12)
        (call $req (i32.const 304) (i32.const 29)
                   (i32.const 0) (i32.const 0) (i32.const 0)))
      (i32.store (i32.const 0) (i32.const 1))))

    (if (i32.eq (local.get $s) (i32.const 1)) (then
      ;; connecting is state 0: proceed only once all three settled
      (if (i32.and (i32.and
            (i32.ne (call $stat (i32.load (i32.const 4)) (i32.const 0))
                    (i32.const 0))
            (i32.ne (call $stat (i32.load (i32.const 8)) (i32.const 0))
                    (i32.const 0)))
            (i32.ne (call $stat (i32.load (i32.const 12)) (i32.const 0))
                    (i32.const 0)))
        (then
          (local.set $n (call $poll (i32.load (i32.const 4))
                               (i32.const 2048) (i32.const 512)))
          (if (i32.lt_s (local.get $n) (i32.const 0))
            (then (local.set $n (i32.const 0)))) ;; failed: report empty
          (i32.store8 (i32.const 384)
            (i32.add (i32.const 48)
                     (call $stat (i32.load (i32.const 8)) (i32.const 0))))
          (i32.store8 (i32.const 388)
            (i32.add (i32.const 48)
                     (call $stat (i32.load (i32.const 12)) (i32.const 0))))
          (drop (call $req (i32.const 352) (i32.const 37)
                           (i32.const 2048) (local.get $n) (i32.const 0)))
          (i32.store (i32.const 16)
            (call $wsopen (i32.const 440) (i32.const 23)))
          (i32.store (i32.const 0) (i32.const 2))))))

    (if (i32.eq (local.get $s) (i32.const 2)) (then
      (local.set $a (call $stat (i32.load (i32.const 16)) (i32.const 0)))
      (if (i32.eq (local.get $a) (i32.const 1)) (then
        (drop (call $send (i32.load (i32.const 16))
                          (i32.const 240) (i32.const 5) (i32.const 1)))
        (i32.store (i32.const 0) (i32.const 3))))
      (if (i32.ge_s (local.get $a) (i32.const 2)) (then
        (i32.store (i32.const 0) (i32.const 9)))))) ;; dead end: times out

    (if (i32.eq (local.get $s) (i32.const 3)) (then
      (local.set $n (call $poll (i32.load (i32.const 16))
                          (i32.const 2602) (i32.const 256)))
      (if (i32.gt_s (local.get $n) (i32.const 0)) (then
        (drop (call $send (i32.load (i32.const 16)) (i32.const 2600)
                          (i32.add (local.get $n) (i32.const 2))
                          (i32.const 1)))
        (i32.store (i32.const 0) (i32.const 4))))))

    (if (i32.eq (local.get $s) (i32.const 4)) (then
      (local.set $a (call $stat (i32.load (i32.const 16)) (i32.const 0)))
      (if (i32.ge_s (local.get $a) (i32.const 2)) (then
        (i32.store8 (i32.const 431)
          (i32.add (i32.const 48) (local.get $a)))
        (drop (call $req (i32.const 400) (i32.const 32)
                         (i32.const 0) (i32.const 0) (i32.const 0)))
        (i32.store (i32.const 0) (i32.const 5))))))

    (f32.store (i32.const 4136)
      (f32.convert_i32_s (i32.load (i32.const 0))))))
