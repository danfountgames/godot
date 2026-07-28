# Node naming conventions

- `PascalCase` for node names: `EnemySpawner`, not `enemy_spawner`.
- Name nodes for their role, not their type: `HealthBar`, not `ProgressBar2`.
- Godot's auto-generated suffixes (`Sprite2D2`, `Node3`) mean the node was never
  named; those are the first candidates for a rename.
- Keep names stable: renaming a node breaks `NodePath` references in scripts,
  animations and signal connections that were written against the old name.
