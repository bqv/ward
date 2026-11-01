project('ward', 'c',
  version: '0.0.1',
  meson_version: '>=1.2.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
    'optimization=g',
  ])

libcbor = dependency('libcbor', version: '>=0.10')
libzstd = dependency('libzstd', version: '>=1.4')
wasmtime = dependency('wasmtime')
libcurl = dependency('libcurl')
openssl = dependency('openssl')
ngtcp2 = dependency('libngtcp2', version: '>=1.0')
ngtcp2_crypto = dependency('libngtcp2_crypto_ossl')
wasi = dependency('wasi-sdk')

deps = [libcbor, libzstd, wasmtime, libcurl, openssl, ngtcp2, ngtcp2_crypto]

add_project_arguments('-D_GNU_SOURCE', language: 'c')

sanitize = get_option('sanitize')
if sanitize != 'none'
  san_flags = ['-fsanitize=' + sanitize, '-fno-omit-frame-pointer']
  add_project_arguments(san_flags, language: 'c')
  add_project_link_arguments(san_flags, language: 'c')
endif

inc = include_directories('src')

core = static_library('ward-core',
  files(
    'src/proto/frame.c',
    'src/proto/codec.c',
    'src/proto/sse.c',
    'src/core/util.c',
    'src/core/ids.c',
    'src/core/log.c',
    'src/core/session.c',
    'src/core/approval.c',
    'src/core/turn.c',
    'src/wasm/host.c',
    'src/net/http.c',
    'src/net/pki.c',
    'src/net/quic.c',
  ),
  include_directories: inc,
  dependencies: deps,
  install: false,
)

ward = executable('ward',
  files('src/cli/main.c', 'src/daemon/server.c'),
  include_directories: inc,
  link_with: core,
  dependencies: deps,
  install: true,
)

test_env = environment()
test_env.set('WARD_BIN', ward.full_path())

foreach t : [
  ['frame', 'tests/frame_test.c'],
  ['codec', 'tests/codec_test.c'],
  ['log', 'tests/log_test.c'],
  ['session', 'tests/session_test.c'],
  ['sse', 'tests/sse_test.c'],
  ['approval', 'tests/approval_test.c'],
  ['pki', 'tests/pki_test.c'],
  ['quic', 'tests/quic_test.c'],
]
  exe = executable(t[0] + '-test', t[1],
    include_directories: inc,
    link_with: core,
    dependencies: deps,
  )
  test(t[0], exe, timeout: 60)
endforeach

fuzz = executable('fuzz-codec-test', 'tests/fuzz_codec.c',
  include_directories: inc,
  link_with: core,
  dependencies: deps,
)
test('fuzz', fuzz, timeout: 180)

fuzz_sse = executable('fuzz-sse-test', 'tests/fuzz_sse.c',
  include_directories: inc,
  link_with: core,
  dependencies: deps,
)
test('fuzz-sse', fuzz_sse, timeout: 180)

gate = executable('gate-m0-test', 'tests/gate_m0.c',
  include_directories: inc,
  link_with: core,
  dependencies: deps,
)
test('m0', gate, env: test_env, timeout: 120)

wasm_cc = wasi.get_variable('clang')
guest_opt = get_option('guest_profile') == 'debug' ? ['-O0', '-g'] : ['-O2']
guest_flags = ['--target=wasm32-wasip1', '-nostdlib', '-Wl,--no-entry',
               '-Wl,--export-memory'] + guest_opt

guest_echo = custom_target('guest-echo',
  input: 'tests/guest/echo.c',
  output: 'echo.wasm',
  command: [wasm_cc, guest_flags, '@INPUT@', '-o', '@OUTPUT@'],
)
guest_wasi = custom_target('guest-wasi',
  input: 'tests/guest/wasi.c',
  output: 'wasi.wasm',
  command: [wasm_cc, guest_flags, '@INPUT@', '-o', '@OUTPUT@'],
)
guest_importer = custom_target('guest-importer',
  input: 'tests/guest/importer.c',
  output: 'importer.wasm',
  command: [wasm_cc, guest_flags, '@INPUT@', '-o', '@OUTPUT@'],
)
alias_target('guests', guest_echo, guest_wasi, guest_importer)

abi_inc = meson.current_source_dir() / 'modules/abi'
module_engine = custom_target('engine.wasm',
  input: 'modules/engine/engine.c',
  output: 'engine.wasm',
  command: [wasm_cc, guest_flags, '-I', abi_inc, '@INPUT@', '-o', '@OUTPUT@'],
)
module_driver = custom_target('driver.wasm',
  input: 'modules/driver/driver.c',
  output: 'driver.wasm',
  command: [wasm_cc, guest_flags, '-I', abi_inc, '@INPUT@', '-o', '@OUTPUT@'],
)
module_provider = custom_target('provider.wasm',
  input: 'modules/provider/provider.c',
  output: 'provider.wasm',
  command: [wasm_cc, guest_flags, '-I', abi_inc, '@INPUT@', '-o', '@OUTPUT@'],
)
alias_target('modules', module_engine, module_driver, module_provider)

module_env = environment()
module_env.set('WARD_MODULE_DIR', meson.current_build_dir())
turn_test = executable('turn-test', 'tests/turn_test.c',
  include_directories: inc,
  link_with: core,
  dependencies: deps,
)
test('turn', turn_test, env: module_env, timeout: 120,
     depends: [module_engine, module_driver, module_provider])

gate_m1 = executable('gate-m1-test', 'tests/gate_m1.c',
  include_directories: inc,
  link_with: core,
  dependencies: deps,
)
test('m1', gate_m1, env: module_env, timeout: 300,
     depends: [module_engine, module_driver, module_provider])

gate_quic = executable('gate-quic-test', 'tests/gate_quic.c',
  include_directories: inc,
  link_with: core,
  dependencies: deps,
)
test('quic-gate', gate_quic, env: test_env, timeout: 240)

wasm_env = environment()
wasm_env.set('WARD_GUEST_DIR', meson.current_build_dir())
wasm_test = executable('wasm-test', 'tests/wasm_test.c',
  include_directories: inc,
  link_with: core,
  dependencies: deps,
)
test('wasm', wasm_test, env: wasm_env, timeout: 120,
     depends: [guest_echo, guest_wasi, guest_importer])
