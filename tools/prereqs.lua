global none
global <const> require, assert, error, ipairs, print

local fs, http, hash, json = require 'fs', require 'http', require 'hash', require 'json'
local p = require 'tools.project'
local install = require 'tools.install'
local M = {}

local function manifest()
  return assert(json.decode(assert(fs.read('tools/prereqs.json'))))
end

local function download(pkg)
  local filename = pkg.url:match('/([^/]+)$')
  if pkg.id == 'tcl-source' then filename = 'tcl-9.0.3.zip' end
  if pkg.id == 'tk-source' then filename = 'tk-9.0.3.zip' end
  local path = '.tools/downloads/' .. filename
  if fs.exists(path) ~= 'file' or assert(hash.file('sha256', path)) ~= pkg.sha256 then
    print('download ' .. pkg.id)
    local r, e = http.get(pkg.url, {to=path, sha256=pkg.sha256, timeout='15m'})
    if not r then error(e, 0) end
  end
  return path
end

function M.ensure(group)
  p.setup()
  local lock <close> = p.lock()
  assert(fs.mkdir('.tools/downloads'))
  local packages = manifest()
  local groups, by_dest = {}, {}
  for _,pkg in ipairs(packages) do
    if pkg.group == 'build' or pkg.group == group or group == 'all' then
      if not by_dest[pkg.dest] then
        by_dest[pkg.dest] = {}
        groups[#groups+1] = {dest=pkg.dest, packages=by_dest[pkg.dest]}
      end
      local members = by_dest[pkg.dest]
      members[#members+1] = pkg
    end
  end
  for _,bundle in ipairs(groups) do
    install.ensure(bundle.dest, bundle.packages, download)
  end
  -- Dynamic Tcl/Tk build first: its interpreter packages the static runtime.
  require('tools.tcltk').ensure(packages)
end

function M.verify()
  p.setup()
  for _,file in ipairs({p.gcc, p.ar, p.windres, p.tcl, p.tcls, p.wish, p.python}) do p.need(file) end
  print('root ' .. p.root)
  print(p.capture {p.gcc, '--version'}:match('[^\r\n]+'))
  -- gsub returns a count too; the parentheses keep it off the printed line
  print((p.capture {p.python, '--version'}:gsub('%s+$', '')))
  print((p.capture {p.tcl, stdin='puts "Tcl [info patchlevel]"\n'}:gsub('%s+$', '')))
end

return M
