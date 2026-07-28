# Game specification

> Replace this file with your game. Everything the agent builds comes from here, so
> vagueness here becomes guesswork there. Delete any section that does not apply,
> rather than leaving it empty — an empty heading reads as an omission.

## The game in one paragraph

What is it, who plays it, and what makes it worth playing? Write this as though
describing it to someone who will never read the rest of the document.

## The core loop

The repeated thing the player does. Be concrete about the verbs: what the player
perceives, what they decide, what they do, what the game does back, and how that
returns them to the start of the loop.

## Player outcomes

The specification is read as a list of outcomes, not features. Write them the way the
agent must be able to verify them.

Good: "From a fresh launch, a player can begin a new game, understand the immediate
objective without external explanation, perform the core action, receive clear
feedback, and reach the first meaningful success state."

Bad: "Add a main menu and a game manager."

- ...

## Success, failure and recovery

What winning looks like. What losing looks like. What happens next in each case, and
how a player gets back into play.

## Controls and input

Which input methods must work: pointer, keyboard, touch, gamepad. Name the ones that
are required, because each one implies its own real-input test route.

## Presentation and art direction

Mood, palette, typography, animation feel, and what the game must never look like.
Put reference images in `docs/REFERENCES/` and mention them here.

## Audio

What must be heard, and when. Note that an agent cannot listen — anything specified
here will be verified structurally (import, bus, trigger, timing) and the limitation
recorded honestly.

## Resolutions and platforms

The viewports, aspect ratios and orientations that must work, and the platforms that
must be built. Each one becomes a row in `.agent/TEST_MATRIX.md`.

## Content

Levels, encounters, items, text, and how much of each. Say what "complete" means in
countable terms.

## Performance targets

Frame time, load time, memory. If you leave this out, the agent will choose reasonable
targets, record them, and hold itself to those instead.

## Accessibility requirements

Contrast, text size, colour independence, input alternatives, timing tolerances.

## Explicitly out of scope

The most useful section in the document. What should *not* be built, however tempting.

## Acceptance criteria

The conditions under which you would call this finished. If they differ from the
Definition of Done in `AGENTS.md`, say so here — this file wins.
