# Implementation Checklist

Use this checklist to keep feature work local, testable, and aligned with repository architecture.

## Planning

- Name the single owning file or function first.
- State one falsifiable hypothesis about the requested behavior.
- Name one focused validation step that could disconfirm it.

## Editing

- Change the root-cause behavior instead of patching symptoms.
- Keep the OSAL and transport boundaries intact.
- Prefer small adjacent edits over broad rewrites.

## Testing

- Cover at least one success path and one failure path.
- Add transport-drop, timeout, malformed input, or reconnect cases when the feature is stateful.
- Reuse existing mocks before introducing new scaffolding.

## Validation

- Run the narrowest test or build target that exercises the change.
- If no focused executable check exists, use the smallest compile or lint check available.
- Report any unvalidated paths explicitly.