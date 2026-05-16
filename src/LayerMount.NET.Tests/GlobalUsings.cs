// PRD 0004 sub-task 6.4: TempLayerEnvironment moved from the local
// Support namespace into LayerMount.TestShared. A global using keeps the
// existing test files (LayerMountTests.cs, VhdImageTests.cs, ...) working
// without a rash of per-file `using LayerMount.TestShared;` edits.
global using LayerMount.TestShared;
