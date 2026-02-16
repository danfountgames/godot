"""Functions used to generate agent prompt headers during build time."""

import os

import methods


def make_agent_prompts_header(target, source, env):
    with methods.generated_wrapper(str(target[0])) as file:
        for src in map(str, source):
            name = os.path.splitext(os.path.basename(src))[0]

            with open(src, "r", encoding="utf-8") as f:
                content = f.read()

            file.write(
                f"inline constexpr const char *_agent_prompt_{name} =\n"
                f"\t{methods.to_raw_cstring(content)};\n\n"
            )
