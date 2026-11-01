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
wasi = dependency('wasi-sdk')

deps = [libcbor, libzstd, wasmtime]

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
    'src/wasm/host.c',
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

wasm_env = environment()
wasm_env.set('WARD_GUEST_DIR', meson.current_build_dir())
wasm_test = executable('wasm-test', 'tests/wasm_test.c',
  include_directories: inc,
  link_with: core,
  dependencies: deps,
)
test('wasm', wasm_test, env: wasm_env, timeout: 120,
     depends: [guest_echo, guest_wasi, guest_importer])
