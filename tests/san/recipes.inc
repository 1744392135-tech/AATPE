run_one_san() {
  local t="$1"
  case "$t" in
    observability) build_test_san observability "aatpe_observability" "log_stub" ;;
    uevent)        build_test_san uevent        "uevent_monitor"     "log_stub" ;;
    event_monitor) build_test_san event_monitor "event_monitor uevent_monitor aatpe_ml" "log_stub v94_daemon_deps" ;;
    interfaces)    build_test_san interfaces   "aatpe_interfaces aatpe_ml" "v94_daemon_deps log_stub" ;;
    webui_json)    build_test_san webui_json    "webui_json aatpe_ml"   "webui_clusters_deps v94_daemon_deps log_stub scenewire" ;;
    thermal)       export -f run_thermal_namespaced
                   RUN_CMD='run_thermal_namespaced' build_test_san thermal "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff" "thermal_deps log_stub" ;;
    # cli #includes aatpe-cli.cpp (main renamed) which self-provides its own
    # readFile/readInt/aatpeExec/g_cpuCount helpers — so NO v94_daemon_deps stub
    # (would multiply-define them). Only aatpe_ml.cpp is needed (LearningModel).
    # v4.2.9 T1/T2: ui-snapshot needs getuid()==0 + a writable /dev/aatpe/state,
    # so run inside the tmpfs-/dev mount namespace (run_cli_namespaced).
    cli)           export -f run_cli_namespaced
                   RUN_CMD='run_cli_namespaced' build_test_san cli "aatpe_ml aatped_apklabel" "log_stub" ;;
    probe)         build_test_san probe        ""                   "" ;;
    watchdog)      build_test_san watchdog     ""                   "" ;;
    # v4.1.1 hotfix: boot-grace / degraded self-heal / pidfile adoption.
    # Same include-the-.cpp pattern as watchdog above (main renamed inside test).
    watchdog_hotfix) build_test_san watchdog_hotfix ""                "" ;;
    # ---- v4.1.1 F1/F3/H5 merge: 7 new focused hotfix targets -----------------
    # F1 #1/#2: whole-TU include aatped_priority.cpp; stubs live in the test TU.
    rt_restore)        build_test_san rt_restore        ""                   "" "-DAATPE_HOST_TEST" ;;
    # F1 #8/#9: links the REAL hardened aatpeExecArgv from aatped_utils.o.
    exec_timeout)      build_test_san exec_timeout     "aatped_utils"       "" "-DAATPE_HOST_TEST" ;;
    # F1 #19: thermal.o evidence predicates against a fake two-cluster sysfs.
    thermal_clusters)  build_test_san thermal_clusters "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff"    "thermal_deps log_stub" "-DAATPE_HOST_TEST" ;;
    # F1 #2: whole-TU include cold_start_rules.cpp; stubs live in the test TU.
    coldstart_release) build_test_san coldstart_release ""                   "" "-DAATPE_HOST_TEST" ;;
    # F3.1 (v4.3.8): whole-TU include cold_start_rules.cpp; drives the
    # coldStartSetProcIdentityForTest() seam (cold/warm/restart/debounce/rate).
    coldinstance)     build_test_san coldinstance     ""                   "" "-DAATPE_HOST_TEST" ;;
    # F3 #16: gpu_boost devfreq off-path write validation + readback + retry.
    gpu_devfreq_recovery) build_test_san gpu_devfreq_recovery "gpu_boost"    "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    # F3 #24: parseCpuOnlineMask (self-contained parser copy in the test TU).
    ebpf_cpu_mask)     build_test_san ebpf_cpu_mask    ""                   "" ;;
    # F3 #12: poolFeatureForTier normalization (header-only inline in aatpe_ml_hybrid.h).
    ml_pool_feature)   build_test_san ml_pool_feature  ""                   "" ;;
    # ---- v4.1.2 hotfix backflow: 4 isolation tests wired (pure wiring, no
    #      production-semantic change; stubs follow the existing recipes). ----
    # X1 T2: D1100 sparse-tier numPools == distinct-cluster count. Pure header-only
    #   poolFeatureForTier() inline in aatpe_ml_hybrid.h (same shape as ml_pool_feature).
    numpools_d1100)    build_test_san numpools_d1100   ""                   "" ;;
    # X1 T1: per-outcome thermal gate. Calls the real static
    #   LearningModel::measureOutcomeV2 + the two thermal atomics, both defined in
    #   aatpe_ml.o. Reuses the SHARED aatpe_ml.o (aatpe_ml.cpp gates nothing on
    #   AATPE_HOST_TEST, so NO force-recompile that would overwrite it) and the same
    #   thermal_deps/log_stub set the isolated build proved (aatpe_ml.o's only
    #   non-libc external dep is aatpeExec, which thermal_deps provides).
    thermal_gate)      build_test_san thermal_gate     "aatpe_ml"          "thermal_deps log_stub" ;;
    # X1 T3 + watchdog latch: pure inline decision helpers in aatpe-constants.h
    #   (aatpe::watchdog::highLoadShouldDegrade / degradedSelfHeal). Header-only.
    degraded_gate)     build_test_san degraded_gate    ""                   "" ;;
    # X2: links the real aatpe_diag.o under root+SELinux-enforcing fake sysfs. The
    #   test TU defines its own logMsg/atomicWriteFile stubs inline, so NO log_stub
    #   (would multiply-define logMsg). aatpe_diag.o's only non-libc external symbol
    #   is atomicWriteFile, supplied by the test. Writes a relative ./v412_testroot.
    diag_gpu)          build_test_san diag_gpu         "aatpe_diag"        "" ;;
    # ---- v4.2 Batch A merge: 4 new hermetic targets -------------------------
    # A1: SurfaceFlinger/compositor whitelist truth table. Links the real
    # aatped_threadutil.cpp (self-contained, same as the threadutil recipe).
    a1_sf)            build_test_san a1_sf            "aatped_threadutil" "" ;;
    # A2: the "placement" producer joins the schedtune single-writer bridge and
    # participates in max(touch,gpu,placement). Run under a mount namespace with a
    # writable fake /dev/stune node so the bridge adopts it and we read it back.
    a2_boost)         export -f run_a2_boost_namespaced
                      RUN_CMD='run_a2_boost_namespaced' build_test_san a2_boost "gpu_boost" "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    # A3: autocalBigRatio() read/corrupt/fingerprint-miss/garbage/cache. The
    # autocal test seams are gated behind AATPE_HOST_TEST, so aatpe_caps.o is
    # force-recompiled with it. autocal_deps = the batch-A link stub (STATE_DIR +
    # file IO + topology hash) for the narrow autocal path.
    a3_autocal)       build_test_san a3_autocal       "aatpe_caps" "autocal_deps log_stub" "-DAATPE_HOST_TEST" ;;
    # A1: uclampMinForTier mode=3 (SF floor evaluated before the PRIME early
    # return) on the full placement graph. Same link set as `placement` (plus
    # aatped_threadutil, which placement.cpp now calls for the SF whitelist); the
    # aatpeBoostSet/Clear no-op bridge stubs live in placement_deps.cpp.
    a1_placement)     build_test_san a1_placement "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # ---- v4.2 Batch D merge: 3 hermetic targets (D1 cgroup .rec / D2 bpftool
    #      argv hardening / D3 hotplug online-mask decision). Link sets mirror the
    #      proven isolated postfreeze/v42_hotfix/build_d.sh recipes exactly; the
    #      test TUs self-provide RUN_DIR/STATE_DIR/logMsg/caps-writer stubs.
    # D1: recovery .rec cgroup field + promoted_tids backfill + old-.rec compat.
    #     Same link set as the `recovery` recipe (aatpe_recovery only; the test TU
    #     supplies RUN_DIR/STATE_DIR + writeFileAtomic + the caps cgroup writers).
    d1recovery)       build_test_san d1recovery "aatpe_recovery" "" ;;
    # H14: cgroup membership .rec format round-trip.
    h14_cgroup_rec)  build_test_san h14_cgroup_rec "aatpe_recovery" "" ;;
    # H14 e2e: mech parser (v1/v2/bad) + recovery API round-trip.
    h14_cgroup_e2e)  build_test_san h14_cgroup_e2e "aatpe_placement_mech aatpe_recovery" "log_stub" ;;
    # D2: aatpeExecArgv no-shell + exit-status hardening. Links the REAL
    #     aatped_utils.o; utils_deps/log_stub/sprop_stub satisfy its host deps
    #     (no AATPE_HOST_TEST — matches build_d.sh).
    d2exec)           build_test_san d2exec "aatped_utils" "utils_deps log_stub sprop_stub" ;;
    # D3: eBPF hotplug online-mask -> ring-rebuild DECISION core (parseCpuOnlineMask
    #     + EbpfScheduler::onlineMaskSetChanged). Links aatpe_ebpf + subsystem +
    #     aatped_utils (exec/mkdir now referenced by eBPF) + integrity; stubs mirror
    #     build_d.sh d3.
    d3hotplug)        build_test_san d3hotplug "aatpe_ebpf aatpe_subsystem aatped_utils integrity" "ebpf_deps log_stub hybrid_deps utils_deps sprop_stub" ;;
    caps)          build_test_san caps         "aatpe_caps" "log_stub" ;;
    diag)          build_test_san diag         "aatpe_diag"         "log_stub" ;;
    ext_neg_psi)   build_test_san ext_neg_psi  "aatpe_diag"         "log_stub" ;;
    mech_uclamp)   build_test_san mech_uclamp  "aatpe_placement_mech aatpe_caps"  "log_stub" ;;
    coldstart)     build_test_san coldstart    "cold_start_rules aatpe_recovery aatpe_ml"   "placement_deps ml_deps recovery_caps_stub" "-DAATPE_HOST_TEST" ;;
    thermal_hal)   build_test_san thermal_hal  "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff"            "v94_daemon_deps log_stub" ;;
    gpu_boost)     build_test_san gpu_boost    "gpu_boost"          "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    ebpf_classify) build_test_san ebpf_classify "aatpe_ebpf aatpe_subsystem integrity" "ebpf_deps log_stub" ;;
    # spsc_queue: header-only template in aatpe_ebpf.h, no prod/stub deps.
    spsc_queue)    build_test_san spsc_queue   ""                   "" ;;
    # ml: aatpe_ml.cpp is self-contained except for aatpeExec (see ml_deps.cpp).
    ml)            build_test_san ml           "aatpe_ml"           "ml_deps" ;;
    # scoring: scoring.cpp + aatpe_ml.cpp; scoring_deps.cpp provides the
    #   daemon globals scoring.cpp reads (pools, cpuMetrics, cfg, history...).
    #   Do NOT also link v94_daemon_deps (multiple definitions).
    scoring)       build_test_san scoring      "scoring adrc aatpe_ml"    "scoring_deps ml_deps log_stub" ;;
    # placement: placement.cpp + aatpe_ml + the 5 aatpe_ml_hybrid_* pieces;
    #   placement_deps.cpp provides the ~50 daemon globals placement.cpp reads.
    placement)     build_test_san placement    "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # H13: hotplug topology rebuild hardening (reuses placement.o + deps).
    h13_hotplug)   build_test_san h13_hotplug  "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # v4.2.7: VM container guest-game resolution (placement-side).
    vm_guest)      build_test_san vm_guest     "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # P2: per-module independent unit tests (100% module coverage).
    # abtest: self-contained stats TU.
    abtest)        build_test_san abtest        "aatped_abtest"        "log_stub" ;;
    gamescene)     build_test_san gamescene     "aatped_gamescene"     "gamescene_deps log_stub" ;;
    # v4.3.7: arbitration layer (pure part + 4 mech cores; no daemon stubs).
    arbitrator)     build_test_san arbitrator     "aatped_arbitrator mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context" "" "-UAATPED_ARBITRATOR_DAEMON" ;;
    arb_wiring)     build_test_san arb_wiring     "aatped_arbitrator mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context" "" "-UAATPED_ARBITRATOR_DAEMON" ;;
    battery)       build_test_san battery      "aatped_battery"       "battery_deps" ;;
    gpu)           build_test_san gpu          "aatped_gpu aatpe_ml" "gpu_deps ml_deps log_stub" ;;
    audio)         build_test_san audio        "aatped_audio"         "audio_deps log_stub" ;;
    applaunch)     build_test_san applaunch    "aatped_applaunch"     "applaunch_deps log_stub" ;;
    logging)       build_test_san logging      "aatped_logging"       "logging_deps" ;;
    utils)         build_test_san utils        "aatped_utils aatpe_helper_audit"         "utils_deps log_stub sprop_stub" ;;
    # uevent_fallback: daemon-side sysfs polling; reuses event_monitor +
    # uevent_monitor + aatpe_ml graph, v94_daemon_deps supplies pools/counters.
    uevent_fallback) build_test_san uevent_fallback "aatped_uevent event_monitor uevent_monitor aatpe_ml" "v94_daemon_deps log_stub uevent_deps" ;;
    ebpf_agg)      build_test_san ebpf_agg     "aatped_ebpf_agg"      "ebpf_agg_deps log_stub" ;;
    # ML hybrid: one HybridLearner class split across 5 TUs; each test links all
    # 5 (they are indivisible at link time) but focuses on its own file.
    ml_hybrid_main)   build_test_san ml_hybrid_main   "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_train)  build_test_san ml_hybrid_train  "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_predict) build_test_san ml_hybrid_predict "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_persist) build_test_san ml_hybrid_persist "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_alpha)  build_test_san ml_hybrid_alpha  "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    # Federated: one FederatedLearner split across 4 TUs (crypto header-only).
    fed_main)      build_test_san fed_main      "aatpe_federated_main aatpe_federated_aggregate" "log_stub" ;;
    fed_aggregate) build_test_san fed_aggregate "aatpe_federated_main aatpe_federated_aggregate" "log_stub" ;;
    # ROUND-014 P3 Plan B: fed_exchange/fed_security removed (dormant wire path; no TU).
    hmac)         build_test_san hmac          "aatped_hmac"            "log_stub" "-DAATPE_HOST_TEST" ;;
    topology)     build_test_san topology      "aatped_topology aatped_gpu aatpe_ml"  "v94_daemon_deps log_stub topology_deps" ;;
    config)       build_test_san config       "aatped_config aatpe_ml aatped_hmac aatpe_config_schema aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub config_deps hybrid_deps ml_deps" ;;
    state)        build_test_san state        "aatped_state aatpe_ml"  "state_deps log_stub ml_deps" ;;
    control)      build_test_san control      "aatped_control aatpe_ml aatpe_command_dispatcher aatpe_command_queue aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "v94_daemon_deps log_stub control_deps hybrid_deps gamelist_stub" ;;
    # round-4 P1 #1/#2: real command queue directory. Same link set as
    # control (drain dispatches through the real executeControlLine()).
    cmdq)         build_test_san cmdq         "aatpe_command_queue aatped_control aatpe_ml aatpe_command_dispatcher aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "v94_daemon_deps log_stub control_deps hybrid_deps gamelist_stub" ;;
    thread_scan)  build_test_san thread_scan  "thread_scan aatpe_ml aatped_threadutil" "v94_daemon_deps log_stub thread_deps" ;;
    threadutil)   build_test_san threadutil   "aatped_threadutil"             "" ;;
    frame)        build_test_san frame        "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel"        "log_stub frame_deps" ;;
    # v4.2.1 hotfix backflow: game-layer recognition (sfListLayers/selectGameLayer/
    # sampleGameLayer/parseGameOverridesContent/parsePmPackageList). Same prod/stub link
    # set as the `frame` recipe (real frame.cpp TU + frame_deps/log_stub); pure
    # aatpe::frameaware functions, no daemon IO. 37 assertions.
    game_layer)   build_test_san game_layer   "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel"        "log_stub frame_deps" ;;
    # v4.2.1 T3: per-app rolling frame stats accessor. Same link set as frame.
    per_app_frame) build_test_san per_app_frame "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel"    "log_stub frame_deps" ;;
    webui_http)   build_test_san webui_http   "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel"   "log_stub webui_deps gamelist_stub" ;;
    # v4.2.9 F-01: webui authorization regression (stok=SSE-only, write=gated).
    # Identical link set to webui_http; pins the F-01 hardening.
    webui_authz)  build_test_san webui_authz "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel"   "log_stub webui_deps gamelist_stub" ;;
    ebpf)         build_test_san ebpf         "aatpe_ebpf aatpe_subsystem"            "log_stub ebpf_deps hybrid_deps" ;;
    # round-2: ML drift detector + topology sidecar (#38/#39/#93).
    v343_ml)      build_test_san v343_ml      "aatpe_ml"             "ml_deps log_stub" ;;
    # v4.3.4 F-group regression: F-001 stale alpha decay + F-003 v1 latch clear.
    f_v434_ml)    build_test_san f_v434_ml    "aatpe_ml"             "ml_deps log_stub" ;;
    # v4.3.4 F-group regression: F-002 hybrid tick thermal pause gate.
    f_v434_hybrid) build_test_san f_v434_hybrid "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    # v4.3.4 B/D regression targets (mirror v94 recipes).
    spillover_rt)  build_test_san spillover_rt "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    state_ttl_d)       build_test_san state_ttl_d       "aatpe_state_ttl"              "ttl_deps" ;;
    d_arithmetic)     build_test_san d_arithmetic     ""                              "" ;;
    # round-2: placement hysteresis gate (#34/#82/#91/#92/#93).
    # Same link set as the placement recipe (placement_deps supplies globals).
    v343_placement) build_test_san v343_placement "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" ;;
    # round-4: #61 session token, #89 fault-injection graceful degradation,
    # #90 100x restart stability simulation. Links the self-contained session store.
    v342_round4)  build_test_san v342_round4  "aatpe_session_token"    "log_stub" ;;
    # round-5 (final): unified subsystem lifecycle/health, StopToken,
    # safe-mode, self-observation, FD/thread-leak + BPF failure-injection tests.
    # Links the self-contained aatpe_subsystem.cpp; no daemon deps.
    v342_round5)  build_test_san v342_round5  "aatpe_subsystem"   "log_stub" ;;
    # release-convergence item 22: Profile arbitration generation-guard stress.
    # Header-only manager + own loadProfile() stub; no production objects needed.
    profile_arb)  build_test_san profile_arb  ""   "" ;;
    # round-3 (#27): profile dedup + whitelist + high-concurrency storm.
    # Header-only manager with own loadProfile() stub; no production objects.
    profile_stress) build_test_san profile_stress ""   "" ;;
    # round-3 (#25): unified recovery module restart/orig-guard. Links
    # aatpe_recovery.cpp; the test supplies RUN_DIR + logMsg + writeFileAtomic stubs.
    recovery)     build_test_san recovery      "aatpe_recovery"   "" ;;
    # main: aatped_main.cpp defines main(); strip that symbol after compiling so
    # test_main.cpp's own main wins, then link with v94_daemon_deps.
    main)         prod_obj_san aatped_main && objcopy --strip-symbol=main "$OUT/aatped_main.o"
                  $CXX $PFLAGS $INCLUDES -DAATPED_ARBITRATOR_DAEMON -c "$SRC/aatped_arbitrator.cpp" -o "$OUT/aatped_arbitrator.o"
                  build_test_san main          "aatped_main aatped_battery aatpe_ml aatpe_ebpf aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatpe_federated_main aatpe_federated_aggregate aatped_ebpf_agg event_monitor uevent_monitor frame_binder adrc integrity aatpe_state_ttl aatpe_helper_audit aatpe_managers aatpe_config_schema aatpe_command_dispatcher aatpe_session_token aatped_priority aatpe_recovery aatpe_command_queue aatpe_subsystem cold_start_rules gpu_boost input_monitor aatped_psi psi_decision aatpe_placement_mech touch_uclamp touch_uclamp_act aatped_arbitrator pre_boost_predict thermal_headroom mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context" "log_stub main_deps touch_host_stub hybrid_deps ebpf_deps recovery_caps_stub scenewire" ;;
    # B-005: focused module unit tests (integrity / adrc / frame_binder / config_schema).
    # integrity: self-contained SHA-256 + chained manifest; needs STATE_DIR/writeFile stubs.
    integrity)    build_test_san integrity     "integrity"                       "integrity_deps log_stub" ;;
    # adrc: pure ADRC controller; only LOG_INFO -> logMsg.
    adrc)         build_test_san adrc          "adrc"                            "log_stub" ;;
    # frame_binder: test #includes frame_binder.cpp to reach the file-static
    # parseLatencyOutput() (probe/watchdog pattern). Do NOT also list frame_binder
    # as a prod object (would multiply-define the static globals).
    frame_binder) build_test_san frame_binder ""                                "frame_deps log_stub" ;;
    # config_schema: self-contained field table + validation/clamping.
    config_schema) build_test_san config_schema "aatpe_config_schema"              "log_stub" ;;
    # apklabel: resolve an installed APK's real <application android:label> out of
    # its own binary resources (AXML + resources.arsc), hostile-input safe. Test
    # TUs embed 14 fixtures and use mkdtemp; no /data reads, no stubs needed.
    apklabel)      build_test_san apklabel      "aatped_apklabel"                 "" ;;
    # h6 integration: new isolation targets (mirror v94 recipes exactly).
    aatpe_iso)          build_test_san aatpe_iso          "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    # v4.4.1 P0: windowing parse + epoch age + all-users enum + heal decision.
    p0_v441)            build_test_san p0_v441            "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel placement_heal_decision" "log_stub frame_deps" ;;
    # C1/C3 exploration tests (default-off features, pure logic host-testable).
    preboost)           build_test_san preboost           "pre_boost_predict"             "" "-DAATPE_HOST_TEST" ;;
    thermheadroom)      build_test_san thermheadroom      "thermal_headroom"             "" "-DAATPE_HOST_TEST" ;;
    # W5 scene_infer + C2 envelope + wiring (pure functions, host-testable; default-off).
    scene_infer)        build_test_san scene_infer        "scene_infer c2_envelope"        "" "-DAATPE_HOST_TEST" ;;
    scene_wiring)       build_test_san scene_wiring       "scene_infer c2_envelope scene_infer_wiring" "" "-DAATPE_HOST_TEST" ;;
    # v4.4.4 C4 cgroup v2 freezer (pure decision/parsing, host-testable; default-off).
    c4_freezer)         build_test_san c4_freezer         "c4_freezer" "" "-DAATPE_HOST_TEST" ;;
    webui_cors)         build_test_san webui_cors         "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel" "log_stub webui_deps gamelist_stub" ;;
    psi_fallback)       build_test_san psi_fallback       "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    thermzone)          build_test_san thermzone          ""                              "" ;;
    thermzone_fixtures) build_test_san thermzone_fixtures ""                              "" ;;
    *) echo "unknown test: $t (authoritative set: observability uevent event_monitor interfaces webui_json thermal cli probe watchdog; spsc_queue/ml/scoring/placement exist only as SKIPPED recipes)" ; return 1 ;;
  esac
}
