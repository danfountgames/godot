def can_build(env, platform):
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "LiveMountDebug",
        "ProjectDomainManager",
        "ScriptDomainManager",
        "AutoloadSessionManager",
        "ProjectSettingsLayerManager",
        "ResourceDomainManager",
        "ImportSessionManager",
        "LaunchController",
        "LiveMountShell",
        "SyncServer",
    ]


def get_doc_path():
    return "doc_classes"
