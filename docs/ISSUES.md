-------------------- ISSUES ------------------------

- why does live orchestrator take sl_pct and tp_pct?

- what is a degraded book?

- we gotta break up mokuren: 
 - the wfa and grid search stuff in its own class
 - idk what the rest of mokuren does but we need it broken up and renamed to something that
   makes sense

- wir haben den selben code an mehreren stellen in wfa live run grid search and stuff, is this the cleanest way to do it? prolly not

"This avoids copying the (large) L2 snapshot per tick, but it creates a strict
lifetime contract: a context is valid only while the referenced objects are
alive and not mutated by a ring-buffer push. Strategies and filters must treat
it as ephemeral." wtf is ephemeral

- how is l2 data loaded into the engine? does it calculate from delta or do we properly get the depth book with levels and order movements like we get from live market? i dont understand that part





