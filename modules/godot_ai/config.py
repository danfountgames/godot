def can_build(env, platform):
    # The AI tooling service drives the editor, so it only exists in editor builds.
    return env.editor_build


def configure(env):
    pass
