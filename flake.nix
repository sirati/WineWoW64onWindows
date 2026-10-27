{
  description = "Wine 11 WoW64 on Windows, Linux MinGW cross build";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/d3b13ad75cc492690728f9307e443dec9ef9a20e";
  outputs = { self, nixpkgs }: let
    system = "x86_64-linux";
    pkgs = import nixpkgs { inherit system; };
    runtime = pkgs.callPackage ./nix/runtime.nix { project = self; };
  in {
    devShells.${system}.default = pkgs.mkShell {
      packages = [ pkgs.python3 ];
    };
    packages.${system} = {
      default = runtime;
      corpus = pkgs.callPackage ./nix/corpus.nix { };
      image-mapping-diagnostic = pkgs.callPackage ./nix/image-diagnostic.nix { };
      continuation-diagnostic = pkgs.callPackage ./nix/continuation-diagnostic.nix { };
      shadow-stack-diagnostic = pkgs.callPackage ./nix/shadow-stack-diagnostic.nix { };
      wow64-policy-diagnostic = pkgs.callPackage ./nix/wow64-policy-diagnostic.nix { };
    };
    apps.${system}.default = {
      type = "app";
      meta.description = "Cross-build and copy the Windows WoW64 runtime";
      program = "${pkgs.writeShellScript "wine-wow64-cross-build" ''
        set -eu
        destination="''${1:-wine-wow64-windows}"
        ${pkgs.coreutils}/bin/mkdir -p "$destination"
        ${pkgs.coreutils}/bin/cp -R --no-preserve=mode ${runtime}/. "$destination/"
        printf 'Windows runtime copied to %s\n' "$destination"
      ''}";
    };
  };
}
