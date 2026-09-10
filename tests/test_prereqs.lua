-- Exercise the actual setup recipe in disposable miniature projects.
global none
global <const> require, assert, error, ipairs, print, tostring, tonumber, table

local rt, fs, proc, json, hash, archive, sched = require 'rt', require 'fs', require 'proc', require 'json', require 'hash', require 'archive', require 'sched'
rt.root(assert(fs.cwd()))
local p = require 'tools.project'
p.setup()
local checks = 0
local function check(ok, why)
  assert(ok, why)
  checks = checks + 1
  print('ok ' .. why)
end

p.scratch(function(work)
  local root = fs.join(work, 'project')
  local served = fs.join(work, 'server')
  assert(fs.mkdir(root .. '/tools')); assert(fs.mkdir(served))
  for _,name in ipairs({'project', 'prereqs', 'install'}) do
    assert(fs.copy('tools/' .. name .. '.lua', root .. '/tools/' .. name .. '.lua'))
  end
  -- This fixture owns only small archives; Tcl's real build is tested by the
  -- cold-checkout exercise, not rebuilt for every fault-injection test.
  assert(fs.write(root .. '/tools/tcltk.lua', 'return {ensure=function() end}\n'))
  assert(fs.copy(rt.exe, root .. '/kuu.exe'))
  assert(fs.write(root .. '/tasks.lua', [[
global none
global <const> require
require('task') 'prereqs' {run=function() require('tools.prereqs').ensure('all') end}
]]))
  local function pack(name, files)
    local src = served .. '/' .. name
    assert(fs.mkdir(src .. '/pkg'))
    for _,file in ipairs(files) do
      assert(fs.mkdir(fs.dirname(src .. '/pkg/' .. file[1])))
      assert(fs.write(src .. '/pkg/' .. file[1], file[2]))
    end
    local path = served .. '/' .. name .. '.zip'
    assert(archive.pack(path, src, {'pkg'}))
    return assert(hash.file('sha256', path))
  end
  local deep_file = ('long-folder/'):rep(24) .. 'data.txt'
  local base = pack('base', {{'tool.txt','GOOD v1'}, {'shared.txt','base'}, {'obsolete.txt','old'}, {deep_file,'deep data'}})
  local overlay = pack('overlay', {{'shared.txt','overlay'}, {'nested/config.ini','a=1'}})
  local next_version = pack('next', {{'tool.txt','GOOD v2'}, {'shared.txt','next'}})
  assert(fs.write(served .. '/bad.zip', 'incorrect upstream bytes'))
  local server <close> = assert(proc.start {p.python, p.path('tests/prereqs_server.py'), served, stream=true, timeout='5m'})
  local port = assert(tonumber(assert(server:read('line','10s')):match('^PORT (%d+)$')))
  local url = 'http://127.0.0.1:' .. port .. '/'
  local packages = {
    {id='base',group='build',dest='.tools/sdk',strip=1,sha256=base,url=url .. 'base.zip'},
    {id='overlay',group='build',dest='.tools/sdk',strip=1,sha256=overlay,url=url .. 'overlay.zip'},
  }
  local function manifest() assert(fs.write(root .. '/tools/prereqs.json', json.encode(packages))) end
  manifest()
  local function spec()
    return {root .. '/kuu.exe','run','--json','prereqs',cwd=root,timeout='45s',
      env={PATH='X:/absent',PYTHONHOME='X:/absent',GCC_EXEC_PREFIX='X:/absent',Z_ROOT='X:/absent'}}
  end
  local function run(success)
    local r = assert(proc.run(spec()))
    local record = json.decode(r.out)
    assert(r.status == 'exit' and record and record.ok == success and (r.code == 0) == success,
      'unexpected setup result: ' .. r.code .. '\n' .. r.out .. r.err)
    return r
  end
  local function file(name) return root .. '/.tools/sdk/' .. name end
  run(true)
  check(fs.read(file('tool.txt')) == 'GOOD v1', 'empty tools directory provisions from verified HTTP downloads')
  check(#file(deep_file) > 260 and fs.read(file(deep_file)) == 'deep data', 'installation inventory verifies paths longer than MAX_PATH')
  check(fs.read(file('shared.txt')) == 'overlay', 'packages sharing a destination retain overlay order')
  local before = assert(fs.read(file('.kuu-installed.json')))
  local repeat_run = run(true)
  check(not (repeat_run.out .. repeat_run.err):find('unpack',1,true) and fs.read(file('.kuu-installed.json')) == before,
    'an unchanged installation is reused without overlay repair loops')
  assert(fs.write(file('tool.txt'), 'BAD! v1'))
  run(true)
  check(fs.read(file('tool.txt')) == 'GOOD v1', 'same-size corruption is detected and repaired')
  assert(fs.remove(file('nested/config.ini')))
  run(true)
  check(fs.read(file('nested/config.ini')) == 'a=1', 'a missing installed file is repaired')
  for _,bad_record in ipairs({'{', '{}', '{"files":false}', '{"files":[]} '}) do
    assert(fs.write(file('.kuu-installed.json'), bad_record))
    run(true)
  end
  check(fs.read(file('tool.txt')) == 'GOOD v1', 'malformed and incomplete installation records rebuild instead of raising')

  -- Correct key but corrupt inventory shape: do not trust an empty/invalid list.
  local record = assert(json.decode(assert(fs.read(file('.kuu-installed.json')))))
  record.files = false
  assert(fs.write(file('.kuu-installed.json'), json.encode(record)))
  run(true)
  check(fs.read(file('shared.txt')) == 'overlay', 'a matching pin with an invalid inventory is not treated as complete')

  assert(fs.write(root .. '/.tools/downloads/base.zip', 'partial cache'))
  assert(fs.remove(file('tool.txt')))
  run(true)
  check(hash.file('sha256', root .. '/.tools/downloads/base.zip') == base and fs.read(file('tool.txt')) == 'GOOD v1',
    'a corrupt cached archive is downloaded again and verified')
  packages[1].url = url .. 'bad.zip'; manifest()
  local rejected = run(false)
  check((rejected.out .. rejected.err):find('mismatch',1,true) and fs.read(file('tool.txt')) == 'GOOD v1'
    and not fs.exists(root .. '/.tools/downloads/bad.zip'), 'wrong upstream bytes fail without replacing working tools')

  packages[1].url, packages[1].sha256 = url .. 'slow.zip', next_version; manifest()
  local child <close> = assert(proc.start(spec()))
  local deadline = sched.clock() + 15
  while not fs.exists(served .. '/started') and child:running() and sched.clock() < deadline do sched.sleep('10ms') end
  assert(fs.exists(served .. '/started'), 'interrupted download did not begin')
  child:kill(); assert(child:wait('10s'))
  check(fs.read(file('tool.txt')) == 'GOOD v1' and not fs.exists(root .. '/.tools/downloads/slow.zip'),
    'killing setup during a response preserves the old installation and rejects partial cache bytes')
  assert(fs.write(served .. '/continue','yes'))
  run(true)
  check(fs.read(file('tool.txt')) == 'GOOD v2' and fs.read(file('shared.txt')) == 'overlay'
    and not fs.exists(file('obsolete.txt')), 'retry after interruption installs the new version and removes obsolete files')

  local stage_name = hash.sum('sha256','.tools/sdk'):sub(1,16)
  local stage = root .. '/.tools/staging/' .. stage_name
  local backup = stage .. '-old'
  assert(fs.mkdir(stage)); assert(fs.write(stage .. '/partial','not a complete install'))
  assert(fs.rename(root .. '/.tools/sdk', backup))
  run(true)
  check(fs.read(file('tool.txt')) == 'GOOD v2' and not fs.exists(stage) and not fs.exists(backup),
    'a kill between directory renames restores the previous complete tree')

  server:kill(); assert(server:wait('10s'))
  run(true)
  check(fs.read(file('tool.txt')) == 'GOOD v2', 'installed tools remain usable with the download server offline')
  assert(fs.remove(file('tool.txt')))
  run(true)
  check(fs.read(file('tool.txt')) == 'GOOD v2', 'offline repair uses verified cached archives')
  local moved = fs.join(work, 'Time Actual moved')
  assert(fs.rename(root, moved)); root = moved
  run(true)
  check(fs.read(file('tool.txt')) == 'GOOD v2', 'a moved project with spaces retains relative installation records')

  assert(fs.remove(file('tool.txt')))
  local first = assert(proc.start(spec()))
  local second = assert(proc.start(spec()))
  local a, b = assert(first:wait('45s')), assert(second:wait('45s'))
  first:close(); second:close()
  check(a.code == 0 and b.code == 0 and fs.read(file('shared.txt')) == 'overlay', 'concurrent setup commands serialize safely')
end)
print('prerequisite recovery: ' .. checks .. ' checks passed')
