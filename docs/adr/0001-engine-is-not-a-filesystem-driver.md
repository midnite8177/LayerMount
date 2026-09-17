# The engine is not a filesystem driver

Overlay semantics (copy-up, whiteouts, layer priority, layer images) and the presentation of a volume are separate problems with separate release cadences and licenses. The engine owns only the semantics, links only system and CRT libraries, and has no mount lifecycle; a host adapter in another repository binds it to a filesystem host. This keeps the engine free of any one driver's install, license, and update story, at the cost of a second repository for every adapter.
