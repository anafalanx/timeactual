global none
global <const> require, assert, error, ipairs, print, tostring, tonumber, table

local rt, task, fs, proc = require 'rt', require 'task', require 'fs', require 'proc'
local p, prereqs, build = require 'tools.project', require 'tools.prereqs', require 'tools.build'
local major, minor = rt.version:match('^(%d+)%.(%d+)$')
assert(major and (tonumber(major) > 0 or tonumber(minor) >= 5),
  'Time Actual requires Kuu 0.5 or newer (0.7 recommended); place kuu.exe in the project root')
local legacy = rt.version == '0.5' -- Retained for projects still using the 0.5 release.

task 'prereqs' {
  desc='Fetch pinned tools and build Tcl/Tk locally',
  args={{'--all', type='flag', help='Also fetch screenshot, code generation and signing tools'}},
  run=function(opts) prereqs.ensure(opts.all and 'all' or 'build') end,
}
for _,group in ipairs({'ui', 'codegen', 'sign'}) do
  task('prereqs-' .. group) {hidden=true, run=function() prereqs.ensure(group) end}
end
task 'env' {desc='Show and verify the project-local toolchain', deps={'prereqs'}, run=prereqs.verify}
task 'build' {
  desc='Compile and package dist/TimeActual.exe', deps={'prereqs'},
  args={{'--debug', type='flag', help='Keep debug symbols'}, {'--out', type='string', help='Output executable'}},
  run=build.build,
}
task 'unit' {desc='Compile and run C engine unit tests', deps={'prereqs'}, run=build.unit}
task 'check' {
  desc='Self-test an existing application in an isolated data directory',
  args={{'exe', type='string', help='Executable, default dist/TimeActual.exe'},
    {'--timeout', type='duration', default='90s', min=legacy and 1 or 0.001, help='Self-test deadline'}}, run=build.check,
}
task 'test' {desc='Build and run task, setup recovery, engine and application tests', deps={'test-tasks', 'test-prereqs', 'build', 'unit', 'check'},
  run=legacy and function() end or nil}
task 'test-tasks' {
  desc='Test task failures, deadlines, JSON output, and environment isolation', deps={'prereqs'},
  run=function()
    p.setup()
    return p.exec {rt.exe, p.path('tests/test_tasks.lua'), timeout='3m'}
  end,
}

task 'test-prereqs' {
  desc='Test cold setup, damaged tools, interrupted downloads, offline repair and relocation', deps={'prereqs'},
  run=function()
    p.setup()
    return p.exec {rt.exe, p.path('tests/test_prereqs.lua'), timeout='5m'}
  end,
}
task 'repackage' {
  desc='Repackage Tcl/UI edits without recompiling', deps={'prereqs'},
  args={{'--out', type='string', help='Output executable'}}, run=build.repackage,
}
task 'build-ext' {desc='Build the window capture extension', deps={'prereqs-ui'}, run=build.extension}
task 'run' {
  desc='Run Time Actual under Kuu supervision (Ctrl-C stops the process tree)',
  args={{'--dev', type='flag', help='Run lunar.tcl under the local wish interpreter'}},
  run=function(opts)
    p.setup()
    if opts.dev then prereqs.ensure('build'); return p.exec({p.wish, p.path('lunar.tcl')}, true) end
    return p.exec({p.need(p.exe)}, true)
  end,
}
task 'icon' {
  desc='Regenerate the application icons', deps={'prereqs'},
  args={{'--face', type='string'}, {'--ink', type='string'}, {'--accent', type='string'}},
  run=function(opts)
    p.setup()
    local lock <close> = p.lock()
    local cmd={p.tcl, p.path('tools/icon.tcl'), '--out', p.path('assets'), '--preview', p.path('build/icon')}
    for _,key in ipairs({'face', 'ink', 'accent'}) do
      if opts[key] then cmd[#cmd+1]='--'..key; cmd[#cmd+1]=opts[key] end
    end
    return p.exec(cmd)
  end,
}
local states={'trusted', 'degraded', 'wide', 'stopped', 'acquiring', 'settings', 'eventlog', 'eventlog-filtered', 'eventlog-sorted'}
local function screenshot(opts, staged)
  p.setup()
  local lock <close> = p.lock()
  local out=p.path(opts.out)
  assert(fs.mkdir(fs.dirname(out)))
  p.scratch(function(dir)
    local cmd={p.tcl, p.path('tools/shot.tcl')}
    if staged then
      cmd=p.args(cmd, {p.wish, p.path('tools/uishot.tcl'), out, opts.state})
    elseif opts.dev then cmd=p.args(cmd, {p.wish, p.path('lunar.tcl'), out})
    else cmd=p.args(cmd, {p.need(p.exe), '-', out}) end
    cmd.env={LUNAR_DATA_DIR=dir, LUNAR_NO_SINGLE_INSTANCE='1'}
    if staged and opts.state=='settings' then cmd.env.LUNAR_SHOT_TITLE='Time Actual Settings' end
    if staged and opts.state:match('^eventlog') then cmd.env.LUNAR_SHOT_TITLE='Time Actual — Event Log' end
    cmd.timeout='90s'
    p.exec(cmd)
  end)
end
task 'shot' {
  desc='Capture the built application window', deps={'build-ext'},
  args={{'out', type='string', required=true}, {'--dev', type='flag'}},
  run=function(opts) screenshot(opts, false) end,
}
task 'uishot' {
  desc='Capture a deterministic UI state', deps={'build-ext'},
  args={{'out', type='string', required=true}, {'state', type='string', default='trusted', choices=states}},
  run=function(opts) screenshot(opts, true) end,
}
task 'test-shot' {
  desc='Test the screenshot pixel converter', deps={'prereqs-ui'},
  run=function() p.setup(); return p.exec {p.tcl, p.path('tools/shot.tcl'), '--selftest'} end,
}
task 'live-nts' {
  desc='Compile and run the opt-in live NTS network probe', deps={'prereqs'},
  args={{'--build-only', type='flag', help='Compile the probe without making network requests'}},
  run=function(opts)
    p.setup()
    local lock <close> = p.lock()
    local cmd={p.python, p.path('tests/run_live_nts.py'), timeout='5m'}
    if opts['build-only'] then cmd[#cmd+1]='--build-only' end
    return p.scratch(function(dir)
      cmd.env={LUNAR_DATA_DIR=dir}
      return p.exec(cmd)
    end)
  end,
}
task 'probe-nts' {
  desc='Run the diagnostic Python NTS-KE/SNTP probe', deps={'prereqs'},
  args={{'hosts', type='string', rest=true, help='Hosts; defaults to the probe\'s PTB set'}},
  run=function(opts)
    p.setup()
    local cmd=p.args({p.python, p.path('scripts/probe_nts.py')}, opts.hosts)
    cmd.timeout='3m'
    return p.exec(cmd)
  end,
}
task 'gen-tz' {
  desc='Regenerate the embedded timezone data from vendored inputs', deps={'prereqs'},
  run=function() p.setup(); local lock <close> = p.lock(); return p.exec {p.python, p.path('scripts/gen_tz_embed.py')} end,
}
task 'gen-win-tzmap' {
  desc='Regenerate the Windows/IANA timezone mapping', deps={'prereqs-codegen'},
  run=function()
    p.setup()
    local lock <close> = p.lock()
    return p.exec {p.path('.tools/go/bin/go.exe'), 'run', p.path('scripts/gen_win_tzmap.go')}
  end,
}
task 'sign' {
  desc='Sign and verify an existing executable with SimplySign', deps={'prereqs-sign'},
  args={{'exe', type='string', help='Executable, default dist/TimeActual.exe'}},
  run=function(opts)
    p.setup()
    local lock <close> = p.lock()
    local sdk=assert(fs.glob('.tools/winsdk/bin/*/x64/signtool.exe'))
    assert(#sdk==1, 'expected the pinned Windows SDK signtool')
    local tool=p.path(sdk[1])
    local exe=p.need(p.path(opts.exe or p.exe))
    p.exec {tool, 'sign', '/a', '/tr', 'http://time.certum.pl', '/td', 'sha256', '/fd', 'sha256', '/v', exe, timeout='3m'}
    local output=p.capture {tool, 'verify', '/pa', '/all', '/v', exe}
    print(output)
    assert(output:lower():find('timestamp', 1, true), 'signature verification reported no timestamp')
    local sha=assert(require('hash').file('sha256', exe))
    assert(fs.write(exe .. '.sha256', sha .. '  ' .. fs.basename(exe) .. '\n'))
    print('sha256 ' .. sha)
  end,
}
task 'clean' {
  desc='Remove generated build and dist directories; keep local tools',
  run=function()
    p.setup()
    local lock <close> = p.lock()
    p.remove('build'); p.remove('dist')
  end,
}
task.default 'test'
