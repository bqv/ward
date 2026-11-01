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
  ),
  include_directories: inc,
  dependencies: [libcbor, libzstd],
  install: false,
)

ward = executable('ward',
  files('src/cli/main.c', 'src/daemon/server.c'),
  include_directories: inc,
  link_with: core,
  dependencies: [libcbor, libzstd],
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
    dependencies: [libcbor, libzstd],
  )
  test(t[0], exe, timeout: 60)
endforeach

fuzz = executable('fuzz-codec-test', 'tests/fuzz_codec.c',
  include_directories: inc,
  link_with: core,
  dependencies: [libcbor, libzstd],
)
test('fuzz', fuzz, timeout: 180)

fuzz_sse = executable('fuzz-sse-test', 'tests/fuzz_sse.c',
  include_directories: inc,
  link_with: core,
  dependencies: [libcbor, libzstd],
)
test('fuzz-sse', fuzz_sse, timeout: 180)

gate = executable('gate-m0-test', 'tests/gate_m0.c',
  include_directories: inc,
  link_with: core,
  dependencies: [libcbor, libzstd],
)
test('m0', gate, env: test_env, timeout: 120)
