global none
global <const> require, assert, error, ipairs, pairs, print, tostring, table, string, setmetatable

local fs, env, proc, task = require 'fs', require 'env', require 'proc', require 'task'
local hash, sync = require 'hash', require 'sync'
local M = {}
M.root = assert(fs.cwd())
function M.path(p) return assert(fs.absolute(p)) end
M.gcc = M.path('.tools/msys2/ucrt64/bin/gcc.exe')
M.ar = M.path('.tools/msys2/ucrt64/bin/ar.exe')
M.windres = M.path('.tools/msys2/ucrt64/bin/windres.exe')
M.strip = M.path('.tools/msys2/ucrt64/bin/strip.exe')
M.python = M.path('.tools/python/python.exe')
M.tcl = M.path('.tools/tcltk/tcl9/bin/tclsh90.exe')
M.tcls = M.path('.tools/tcltk/tcl9s/bin/tclsh90s.exe')
M.wish = M.path('.tools/tcltk/tcl9/bin/wish90.exe')
M.exe = M.path('dist/TimeActual.exe')

function M.setup()
  -- Only this process and its children change. Nothing is installed globally.
  for _,dir in ipairs({'.tools/home', '.tools/tmp', '.tools/cache', 'build'}) do assert(fs.mkdir(dir)) end
  local inherited = env.all()
  for name in pairs(inherited) do
    local upper = name:upper()
    if upper:match('^Z_') or upper:match('^PYTHON') or upper:match('^TCL') or upper:match('^TK_')
      or upper:match('^GOCACHE') or upper:match('^GOMODCACHE') then env.set(name, nil) end
  end
  for _,name in ipairs({'MSYS2_DIR', 'MSYS2_ARG_CONV_EXCL', 'BASH_ENV', 'ENV', 'GCC_EXEC_PREFIX',
    'COMPILER_PATH', 'LIBRARY_PATH', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH',
    'CC', 'CXX', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'MAKEFLAGS', 'MFLAGS', 'CONFIG_SITE',
    'TCLLIBPATH', 'GOROOT', 'GOPATH', 'GOTOOLCHAIN'}) do env.set(name, nil) end
  local windows = assert(env.get('SystemRoot'), 'SystemRoot is missing')
  env.set('PATH', table.concat({M.path('.tools/tcltk/tcl9/bin'), M.path('.tools/msys2/ucrt64/bin'),
    M.path('.tools/msys2/usr/bin'), fs.join(windows, 'System32'), windows}, ';'))
  env.set('HOME', M.path('.tools/home'))
  env.set('XDG_CACHE_HOME', M.path('.tools/cache'))
  env.set('TEMP', M.path('.tools/tmp')); env.set('TMP', M.path('.tools/tmp'))
  env.set('MSYSTEM', 'UCRT64')
  env.set('MSYS2_PATH_TYPE', 'strict')
  env.set('GOCACHE', M.path('.tools/cache/go-build'))
  env.set('GOMODCACHE', M.path('.tools/cache/go-mod'))
  env.set('GOPATH', M.path('.tools/go-work'))
  env.set('GOTOOLCHAIN', 'local')
  env.set('GOENV', 'off')
  env.set('GOWORK', 'off')
  env.set('GOFLAGS', '')
  env.set('TCL_LIBRARY', M.path('.tools/tcltk/tcl9/lib/tcl9.0'))
  env.set('TK_LIBRARY', M.path('.tools/tcltk/tcl9/lib/tk9.0'))
end

function M.lock()
  local identity = assert(fs.canon(M.root))
  return assert(sync.lock('timeactual-' .. identity.volume .. '-' .. identity.file, '30m'))
end

function M.exec(argv, unbounded)
  if not unbounded then argv.timeout = argv.timeout or '10m' end
  local ok, e = task.exec(argv)
  if not ok then error(e, 0) end
  return true
end

function M.capture(argv)
  argv.timeout = argv.timeout or '10m'
  local r, e = proc.run(argv)
  if not r then error(e, 0) end
  if r.truncated then error('output exceeded its capture limit: ' .. argv[1]) end
  if r.status ~= 'exit' or r.code ~= 0 then
    error(require('err').new('TASK', 'exit', argv[1] .. ': ' .. r.status .. ' ' .. r.code
      .. '\n' .. r.out .. r.err, {exit = r.code ~= 0 and r.code or 1}), 0)
  end
  return r.out
end

function M.args(first, ...)
  local out = {table.unpack(first)}
  for _,list in ipairs({...}) do for _,value in ipairs(list) do out[#out+1] = value end end
  return out
end

function M.need(path)
  assert(fs.exists(path) == 'file', 'missing ' .. path .. '; run .\\kuu.exe run prereqs')
  return path
end

function M.remove(path)
  -- The caller supplies a path under this checkout. Canonical parent identity
  -- prevents a directory junction from redirecting a recursive deletion.
  local abs = M.path(path)
  local rel = assert(fs.relative(abs, M.root))
  assert(rel ~= '.' and not rel:match('^%.%.') and not rel:match('^%a:'), 'refusing removal outside project: ' .. abs)
  local parent = assert(fs.canon(fs.dirname(abs)))
  local root = assert(fs.canon(M.root))
  local actual = assert(fs.relative(parent.path, root.path))
  assert(actual == '.' or (not actual:match('^%.%.') and not actual:match('^%a:')), 'parent leaves project: ' .. abs)
  if fs.exists(abs) then assert(fs.remove(abs, {recursive=true})) end
end

function M.write_if_changed(path, bytes)
  if fs.exists(path) ~= 'file' or assert(fs.read(path)) ~= bytes then assert(fs.write(path, bytes)) end
end

function M.scratch(fn)
  local dir = assert(fs.tempdir {dir='.tools/tmp', prefix='timeactual-'})
  local cleanup <close> = setmetatable({}, {__close=function(_, failure)
    if failure then print('diagnostics kept in ' .. dir) else M.remove(dir) end
  end})
  return fn(dir)
end

return M
