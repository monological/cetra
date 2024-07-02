
~ pcb ~

DSL for schematic capture with bidir updates between render and code. P&R done
from broad strokes to detailed localized routing. Clustering  components by
function. Routing will take in account constraints such as targeted impedance
matching, reduce e&m crosstalk, auto swapping pins if part models allow for it
(e.g., fpga pins). Most likely will be done using A* with cost-code based
heuristics.

Also going to have a feature where you don’t have to commit to connecting a net
to a pin. It can just be a tentative connection to the part. Then the auto
router will connect it intelligently and update the dsl/schems

Part/package management will be totally simplified as well. All you have to do
is screenshot the package in the data sheet, dump it in a directory and name
the file with the package. Tool will extract dimensions using advanced ODR
based on ML


+ component
+ package
+ 


