global none
global <const> require, assert, ipairs, print, tostring

local rt, fs, proc, json = require 'rt', require 'fs', require 'proc', require 'json'
rt.root(assert(fs.cwd()))
local p = require 'tools.project'
p.setup()
local fixture=p.path('build/task tests/fixture.exe')
assert(fs.mkdir(fs.dirname(fixture)))
p.exec {p.gcc, '-std=c23', '-Wall', '-Wextra', '-Werror', '-static',
  p.path('tests/task_fixture.c'), '-o', fixture}
local checks=0
local function check(ok, message)
  assert(ok, message)
  checks=checks+1
end
local function run(args, mode)
  args=p.args({rt.exe}, args)
  args.env={TIMEACTUAL_TASK_FIXTURE=mode or false,
    Z_HOME='X:/absent', Z_ROOT='X:/absent', Z_TCLTK='X:/absent', Z_MSYS2='X:/absent',
    PATH='X:/absent', TCL_LIBRARY='X:/absent', PYTHONHOME='X:/absent',
    GCC_EXEC_PREFIX='X:/absent', CPATH='X:/absent', TEMP='X:/absent', TMP='X:/absent'}
  args.timeout='2m'
  return assert(proc.run(args))
end
local listed=run({'list', '--json'})
check(listed.code==0 and assert(json.decode(listed.out)).ok, 'task declarations/list JSON failed')
local bad=run({'run', '--json', 'build', '--unknown'})
check(bad.code==2 and not assert(json.decode(bad.out)).ok, 'invalid arguments must fail before work starts')
for _,case in ipairs({{'pass',0}, {'exit',23}, {'missing',1}, {'failure',1}}) do
  local r=run({'run', '--json', 'check', fixture}, case[1])
  local record=assert(json.decode(r.out))
  check(r.code==case[2] and record.ok==(case[2]==0),
    'wrong result for '..case[1]..': '..r.code..'\n'..r.out..r.err)
end
local timeout=run({'run', '--json', 'check', fixture, '--timeout', '100ms'}, 'timeout')
check(timeout.status=='exit' and timeout.code~=0 and timeout.elapsed<10,
  'selftest deadline was not enforced: '..assert(json.encode(timeout)))
local survivors={}
for _,entry in ipairs(assert(proc.find {name='fixture'})) do
  if entry.exe and p.path(entry.exe):lower()==fixture:lower() then survivors[#survivors+1]=entry end
end
check(#survivors==0, 'timed-out selftest left a process behind')
local envtest=run({'-e', 'require("tools.project").setup(); local p=require("tools.project"); print(p.capture{p.gcc,"-dumpfullversion"}); print(p.capture{p.python,"--version"})'})
local compiler=p.capture {p.gcc, '-dumpfullversion'}:gsub('%s+$', '')
check(envtest.code==0 and envtest.out:find(compiler,1,true) and envtest.out:find('Python 3.14.6',1,true),
  'tool execution depended on the inherited environment\n'..envtest.err)
print('task integration: '..checks..' checks passed')
