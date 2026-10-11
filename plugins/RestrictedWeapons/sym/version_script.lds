{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* polyhook binding slots (defined by polyhook_bindings, filled by the plugify module
           from the polyhook plugin; the CanAcquire detour runs through them) */
        __polyhook_*;

        /* hudmenu binding slots (defined by hudmenu_bindings, filled by the plugify module from
           the hudmenu plugin's exported GetClientMenuType/SetClientMenuType) */
        __hudmenu_*;

        /* Exported methods (RestrictedWeapons.pplugin, group by group). The consumer headers in
           plugins/RestrictedWeapons/include are generated from the manifest with bin/plugify-gen -
           keep this list in sync. */
        OnWeaponRestricted_Register;
        OnWeaponRestricted_Unregister;

    local: *;
};
