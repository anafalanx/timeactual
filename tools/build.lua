global none
global <const> require, assert, error, ipairs, print, table, tostring

local fs, hash = require 'fs', require 'hash'
local p = require 'tools.project'
local M = {}
local engine = {'app_paths', 'sysvol', 'netutil', 'ntp', 'clock', 'logbuf', 'tz', 'tzif',
  'tz_embed', 'tz_winmap', 'tz_winmap_gen', 'siv', 'nts_ke', 'nts_ef', 'pinned_tls',
  'cert_verify_win', 'pin_store', 'update_check', 'nts', 'dns', 'h2'}
local libs = {'-lnetapi32', '-lkernel32', '-luser32', '-ladvapi32', '-luserenv', '-lws2_32',
  '-lgdi32', '-lcomdlg32', '-limm32', '-lcomctl32', '-lshell32', '-luuid', '-lole32',
  '-loleaut32', '-lwinspool', '-lcrypt32', '-lbcrypt', '-lwtsapi32', '-lwinmm', '-ld2d1', '-ldwrite'}

local function package_app(out)
  out = p.path(out or p.exe)
  local bare = p.need(p.path('build/lunar-bare.exe'))
  assert(fs.mkdir(fs.dirname(out)))
  local stage = out .. '.new'
  p.exec {p.tcls, p.path('tools/package.tcl'), '--wrapper', bare, stage}
  local ok, e = fs.rename(stage, out, {replace=true})
  if not ok and fs.exists(out) == 'file' then
    local parked, pe = fs.rename(out, out .. '.old', {replace=true})
    if not parked then error(pe, 0) end
    ok, e = fs.rename(stage, out)
    if not ok then fs.rename(out .. '.old', out); error(e, 0) end
    print('parked running copy at ' .. out .. '.old')
  elseif not ok then error(e, 0) end
  local sha = assert(hash.file('sha256', out))
  assert(fs.write(out .. '.sha256', sha .. '  ' .. fs.basename(out) .. '\n'))
  print('built ' .. out .. ' (' .. assert(fs.stat(out)).size .. ' bytes)\nsha256 ' .. sha)
end

function M.build(opts)
  p.setup()
  local lock <close> = p.lock()
  assert(fs.mkdir('build'))
  local prereq = p.capture {p.python, p.path('scripts/build.py')}
  print(prereq)
  local archives = assert(fs.glob('build/mbedtls/*/libmbedtls_lunar.a'))
  assert(#archives == 1, 'expected one current mbedTLS archive')
  local inc = p.path('.tools/tcltk/tcl9/include')
  local lib = p.path('.tools/tcltk/tcl9s/lib')
  assert(fs.copy('assets/icon.ico', 'build/lunar.ico', {replace=true}))
  p.exec {p.tcl, p.path('tools/genres.tcl'), p.path('build')}
  -- windres reparses preprocessor arguments itself. Keep them relative and
  -- avoid its shell-based popen route so a checkout path may contain spaces.
  p.exec {p.windres, '--use-temp-file', '--include-dir', '.',
    '--include-dir', '../.tools/tcltk/tcl9/include', 'lunar.rc', '-O', 'coff',
    '-o', 'lunar.res', cwd=p.path('build')}
  local common = {'-std=c23', '-O2', '-ffunction-sections', '-fdata-sections'}
  if opts.debug then common[#common+1] = '-g' end
  local ef = p.args(common, {'-I' .. p.path('third_party/mbedtls-3.6.6/include'),
    '-I' .. p.path('third_party'), '-I' .. p.path('build'), '-DMBEDTLS_CONFIG_FILE=<lunar_mbedtls_config.h>'})
  local objects = {}
  for _,name in ipairs(engine) do
    local obj = p.path('build/' .. name .. '.o')
    print('cc ' .. name .. '.c')
    p.exec(p.args({p.gcc}, ef, {'-c', p.path('src/' .. name .. '.c'), '-o', obj}))
    objects[#objects+1] = obj
  end
  for _,name in ipairs({'lunarx', 'lunarclock', 'lunar_main'}) do
    local flags = p.args(common, {'-DSTATIC_BUILD=1', '-I' .. inc, '-I' .. p.path('src'), '-I' .. p.path('build')})
    if name == 'lunar_main' then flags = p.args(flags, {'-municode', '-DUNICODE', '-D_UNICODE', '-DLUNAR_STATIC_LUNARX'}) end
    local obj = p.path('build/' .. name .. '.o')
    print('cc ' .. name .. '.c')
    p.exec(p.args({p.gcc}, flags, {'-c', p.path('src/' .. name .. '.c'), '-o', obj}))
    objects[#objects+1] = obj
  end
  print('link Time Actual')
  p.exec(p.args({p.gcc, '-municode', '-mwindows', '-static-libgcc', '-Wl,--gc-sections'}, objects,
    {p.path('build/lunar.res'), lib .. '/libtcl9tk90.a', lib .. '/libtcl90.a',
      lib .. '/libtclstub.a', p.path(archives[1])}, libs, {'-o', p.path('build/lunar-bare.exe')}))
  if not opts.debug then p.exec {p.strip, p.path('build/lunar-bare.exe')} end
  package_app(opts.out)
end

function M.repackage(opts)
  p.setup()
  local lock <close> = p.lock()
  package_app(opts.out)
end

function M.unit()
  p.setup()
  local lock <close> = p.lock()
  return p.exec {p.python, p.path('tests/run_tests.py'), timeout='15m'}
end

function M.check(opts)
  p.setup()
  local lock <close> = p.lock()
  local exe = p.need(p.path(opts.exe or p.exe))
  p.remove('build/timeactual-selftest.txt')
  p.scratch(function(dir)
    local report = fs.join(dir, 'selftest.txt')
    -- 0.6 uses seconds throughout; keep published 0.5 CI compatible.
    local timeout = opts.timeout or '90s'
    if opts.timeout and require('rt').version == '0.5' then timeout = tostring(opts.timeout) .. 'ms' end
    local output = p.capture {exe, '--selftest', report, timeout=timeout,
      env={LUNAR_DATA_DIR=dir, LUNAR_NO_SINGLE_INSTANCE='1',
        TCL_LIBRARY=false, TK_LIBRARY=false, TCLLIBPATH=false,
        PATH=fs.join(assert(require('env').get('SystemRoot')), 'System32')}}
    assert(fs.exists(report) == 'file', 'selftest produced no report\n' .. output)
    local text = assert(fs.read(report))
    print(text)
    assert(fs.copy(report, 'build/timeactual-selftest.txt', {replace=true}))
    assert(('\n' .. text):match('\nstatus=ok\r?\n'), 'application selftest failed')
  end)
end

function M.extension()
  p.setup()
  local lock <close> = p.lock()
  p.exec {p.gcc, '-std=c23', '-O2', '-Wall', '-shared', '-DUSE_TCL_STUBS',
    '-I' .. p.path('.tools/tcltk/tcl9/include'), p.path('src/cap.c'), '-o', p.path('build/cap.dll'),
    '-L' .. p.path('.tools/tcltk/tcl9/lib'), '-ltclstub', '-static-libgcc', '-luser32', '-lgdi32'}
end

return M
