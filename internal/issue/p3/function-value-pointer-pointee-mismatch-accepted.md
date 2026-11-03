# Function value: binding loses pointer shape in the saved signature

Also (same review, PRE-EXISTING): binding `int f((int,int)[] q)` or `int f((int,int)** q)` to the matching function<> type fails
"parameter 1 differs" - the parsed signature of f loses the view / second pointer level.
