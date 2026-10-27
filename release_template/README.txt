Shadow Man Remastered - Co-op Mod
=================================

Up to four players play together over Steam (no port forwarding needed):
shared enemies and bosses, shared progress; doors, switches, coffin gates,
cutscenes and pickups are the same for everyone.

Installation (all players)
--------------------------
1. In Steam: right-click "Shadow Man Remastered" -> Manage -> Browse local files.
2. Copy the entire content of this ZIP file into that folder
   (Install.bat, Uninstall.bat and the SMCoop folder).
3. Double-click Install.bat.
Everyone needs EXACTLY the same version of the mod.

Playing
-------
Host (whose adventure is played):
  1. Start Shadow Man, in the main menu "Co-op Lobby" -> "Create lobby".
  2. "Invite friends (Steam)" -> choose up to three friends.
  3. As soon as everyone is listed under "Players": go back and load your adventure.

Guest:
  1. Accept the Steam invite (Shadow Man may start by itself) and wait in the menu.
  2. When the host loads their adventure, the guest is asked - no matter which
     menu they are in - "Host is playing - join with:":
       - "Load your own adventure": pick your own adventure (saved together
         with the host) - cutscenes and areas you have seen stay known.
       - "New Adventure": as before, without your own adventure.
     The guest is then placed at the host once. Whatever the host has and the
     guest is missing (weapons, Gads, souls, progress) is added for the guest.
     Tip: everyone saves at the end of a session, then it fits next time.
  Game crashed? Restart it and click "Join Game" on the host in the Steam
  friends list - or the host invites again: pause menu -> "Invite friends (Steam)".

After that everyone is free. You are only pulled to another player when they
trigger a cutscene or a boss fight.

Settings (main menu -> Co-op Lobby)
-----------------------------------
- Enemy HP per guest (0-200 %, click = +10 %; host only): every guest gives all
  enemies this much more health. Example: 50 % with three players = double HP.
- Shared HP on/off (host only): one shared health bar - if one is hit, everyone
  loses health; if one heals, everyone is healed. If one dies, everyone dies.
- Show names on/off (each player): name, distance and arrow above the other
  players, also through walls; an arrow at the screen edge when someone is off
  screen; at the top you see who is in another level.
  Colours: host green, player 2 red, player 3 blue, player 4 purple.

Teleport
--------
Pause menu -> "Teleport to player" -> choose a player: you land next to them,
even if they are in another level (the level is switched).

Push blocks and Calabash
------------------------
When someone pushes a block, it slides for everyone and is "done" for everyone
afterwards (as in the game: each block only once). Adventures saved with a
block that was moved but still pushable are repaired on loading.
Everyone sees the Calabash: placing it, the fire and the explosion - which hits
anyone standing too close. Destroyed Calabash plates are gone for everyone.

Lifts and ride-along parts
--------------------------
Whoever stands in the lift (or in the cage, on the ring) rides along - even if
another player starts it. Whoever is elsewhere stays put (previously you jumped
along and fell through the world). Sinking pillars in the lava and planks that
break react for everyone when someone stands on them. Traps (blades, hammers,
statues moving back and forth) run at each player's own pace.
Puzzles (drums, switches, flame doors, padlock doors ...): what one player
solves or opens is solved or open for the others too.

Death
-----
Everyone dies on their own, as in the normal game - also in boss fights.
Only with "Shared HP" or in Hardcore does everyone die together. After a death
you land where the game puts you - no jump to the host. To get back to the
others quickly: pause menu -> "Teleport to player".

Hardcore
--------
"New Adventure" -> "Hardcore" or "Hardcore The Horror": if anyone dies -
anywhere -, everyone dies and the saves of this adventure are PERMANENTLY
DELETED (including the quick save). Then it is back to the main menu.

Language: the mod speaks German when the game is set to German, otherwise
English (including the log).

A window "SMCoop" opens next to the game - that is the mod's log (also in
smcoop_log.txt in the game folder, the one from the previous start in
smcoop_log_prev.txt). After a crash please send both files and CRASHLOG.TXT
from the game folder.

Uninstall
---------
Double-click Uninstall.bat. Alternatively in Steam: Properties ->
Installed Files -> Verify integrity of game files.
