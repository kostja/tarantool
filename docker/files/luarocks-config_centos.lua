rocks_trees = {
   { name = [[user]], root = home..[[/.luarocks]] },
   { name = [[system]], root = [[/usr/local]] }
}

lib_modules_path="/lib64/lua/"..lua_version

rocks_servers = {
    [[http://rocks.tarantool.org/]],
    [[http://luarocks.org/repositories/rocks]]
}
