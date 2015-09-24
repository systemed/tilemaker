# -*- mode: ruby -*-
# vi: set ft=ruby :

Vagrant.configure(2) do |config|
  config.vm.box = "ubuntu/trusty64"

  config.vm.provider "virtualbox" do |vb|
    vb.memory = "2048"
  end

  config.vm.provision "shell", inline: <<-SHELL
set -e

apt-get update
apt-get -yy upgrade
apt-get install -yy build-essential liblua5.1-0 liblua5.1-0-dev libprotobuf-dev libsqlite3-dev protobuf-compiler git
add-apt-repository ppa:ostogvin/tjo-develop
apt-get update
apt-get install -yy libboost1.58-all-dev

cd /home/vagrant
sudo -u vagrant git clone https://github.com/rpavlik/luabind.git
cd luabind
sudo -u vagrant patch -Np1 << "PATCH"
diff --git a/Jamroot b/Jamroot
index 94494bf..1dc3a73 100755
--- a/Jamroot
+++ b/Jamroot
@@ -81,12 +81,13 @@ else if [ os.name ] in LINUX MACOSX FREEBSD
         prefix = $(prefix:D) ;
     }
 
-    local lib = $(prefix)/lib ;
+    local lib-prefixes = $(prefix)/lib /usr/lib/x86_64-linux-gnu ;
+    local lib-suffixes = lua51 lua5.1 lua "" ;
 
     local names = liblua5.1 liblua51 liblua ;
     local extensions = .a .so ;
 
-    library = [ GLOB $(lib)/lua51 $(lib)/lua5.1 $(lib)/lua $(lib) :
+    library = [ GLOB $(lib-prefixes)/$(lib-suffixes) :
         $(names)$(extensions) ] ;
     lib-name = [ MATCH "lib(.*)" : $(library[1]:B) ] ;
 
PATCH
sudo -u vagrant bjam variant=release
bjam variant=release install
ldconfig

SHELL
end

