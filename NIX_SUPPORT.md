# NixOS and Nix support

This repo is identical the main repo, except the addition of three files
- `flake.nix`: used to build the vAuth binary on any distro using nix package manager.
- `module.nix`:essentially part of flake.nix that handles the NixOS integration.
- `NIX_SUPPORT.md`: File explaining how to integrate `vAuth` in NixOS.

> [!NOTE]
> This flake is made for x86_64 and aarch64 architecture and linux only (due to dependency of systemd by vAuth)

## Understanding flakes
For those who isn't familiar with flakes,refer the offical wiki [here](https://nixos.wiki/wiki/Flakes).

## For NixOS users

The easiest way to install vAuth with flakes is to include it in your flake.nix file.

For example  if you dont have a flake.nix file, create one in the same directory has configuration.nix (usually `/etc/nixos`)

```bash
cd /etc/nixos
sudo nano flake.nix
```
then paste this below

> replace nixos-<version> with the correct version. usually its the unstable version, `nixos-unstable`, you can replace it with unstable or your current version, eg: `nixos-26.05` in my case.
> Also replace  the `x86_64-linux` with your architechture. For now `x86_64-linux` and `aarch64-linux` builds are given, however aarch64-linux has not been tested in real machine, but its expected to work.

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-<version>";
  };
  outputs = { self, nixpkgs, vauth, ... }:
  let 
    system="x86_64-linux";
  in
  {
    nixosConfigurations.nixos = nixpkgs.lib.nixosSystem {
      inherit system;
      modules = [ 
        ./configuration.nix
      ];
    };
  };
}

```
check if your flakes have any issues using

```bash
sudo nix flake check
```
> If any issues comes up try referring the offical wiki link I gave at the beginning. 

If you made the flake.nix or you already have a flake.nix just do the following....


- Add `vauth.url = "github:R-KL/vAuth/feature/nixos-support";` inside the inputs 
- Add `vauth.nixosModules.default` has one of the modules
- Add  `{ services.vauth.enable = true; }` has one of the modules

then your flake should look like this (compared to example flake
)
```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-<version>";
    vauth.url = "github:R-KL/vAuth/feature/nixos-support"; # Add this
  };
  outputs = { self, nixpkgs, vauth, ... }:
  let 
    system="x86_64-linux"; 
  in
  {
    nixosConfigurations.nixos = nixpkgs.lib.nixosSystem {
      inherit system;
      modules = [ 
        ./configuration.nix
        vauth.nixosModules.default # Add this 
        { services.vauth.enable = true; } # Add this
      ];
    };
  };
}

```

Then rebuild your system using

```bash
sudo nixos-rebuild switch
```

That's it. Next follow the instructions in the README.md file before running the binaries provided by vAuth.

## For Nix users.

This flake can successfully build the `vAuth` application, however, **the systemd integration is not compatible** since its aimed at NixOS. For easy installation consider the deb package.

If you want to build from source, and you have nix installed, it can be done using

```bash

nix build 'github:R-KL/vAuth/feature/nixos-support#vauth'

```

> [!NOTE]
> In the above code, the github link depends on where the file is hosted! It should be changed to correspoding url when merging. example, lets say its hosted in my feature branch feature/nixos-support, then it should be like shown above. But if I moved it to my main repo, it would be `github:R-KL/vAuth#vauth` if I merged to main repo
s
The packages etc are pinned, hence the compilation and build process will be sucessful.

> Advantage of using nix build instead of CMAKE is that it handles the process of downloading slint-UI, generating the CMake build script using ninja and building efforlessly, it also installed all the dependencies, and completes the entire process in one command, which is given above. Cmake is used internally to build.
