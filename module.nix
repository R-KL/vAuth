{
  config,
  lib,
  vauth,
  ...
}:

let
  cfg = config.services.vauth;
in
{
  options.services.vauth = {
    enable = lib.mkEnableOption "vAuth FIDO2 authenticator";

    package = lib.mkOption {
      type = lib.types.package;
      default = vauth;
      description = "The vAuth package to use.";
    };
  };

  config = lib.mkIf cfg.enable {
    # vAuth PAM configuration
    systemd.tmpfiles.rules = [
      "L+ /etc/pam.d/vauth - - - - ${cfg.package}/etc/vauth/config/vauth"
    ];

    # Package-shipped systemd units
    systemd.packages = [
      cfg.package
    ];

    # Package-shipped D-Bus policy
    services.dbus.packages = [
      cfg.package
    ];

    # Package-shipped udev rules
    services.udev.packages = [
      cfg.package
    ];

    # vAuth system user/group
    users.groups.vauth = { };

    users.users.vauth = {
      isSystemUser = true;
      group = "vauth";
      extraGroups = [ "tss" ];
      home = "/var/lib/vauth";
    };

    # vAuth uses /dev/uhid
    boot.kernelModules = [ "uhid" ];

    systemd.services.vauth = {
      wantedBy = [ "multi-user.target" ];
    };

    systemd.sockets.vauth-pam-verifier.wantedBy = [ "sockets.target" ];
    # Make CLI/UI available
    environment.systemPackages = [
      cfg.package
    ];
  };
}
