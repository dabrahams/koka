effect st 
  control get() : int
  control set(i : int)  : ()


fun state( init : int, action : () -> <st|e> a ) : e a 
  with return(f){ f(init) }
  with handler 
    return(x)      { fn(s){ x } }
    rcontrol get() { fn(s){ rcontext.resume(s)(s) } }
    rcontrol set(s){ fn(_){ rcontext.resume(())(s) } }    
  action()

fun counter( c : int ) : _ int {
  val i = get()
  if (i==0) then c else 
    set(i.dec)
    counter(c.inc)
  

fun main() 
  with state(10100)
  counter(0).println
