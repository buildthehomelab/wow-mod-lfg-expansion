void AddLfgExpansionScripts();

// The name is not free: AzerothCore's module CMake generates a call to
// Add<directory name with _ instead of ->Scripts(). The directory is
// mod-lfg-expansion, so Addmod_lfg_expansionScripts().
void Addmod_lfg_expansionScripts()
{
    AddLfgExpansionScripts();
}
