We make use of ElephantTracks to help us track object lifetimes and create our oracle. The final oracle we create is a csv file that contains the allocation and deallocation timestamps for each object. It looks like this:
alloc_seq,free_at_seq,size,type,obj_id
1,8,24,java.net.URL[],853119666

Since we are using ElephantTracks, we can use the alloc_seq and free_at_seq to determine when each object was allocated and deallocated. If we make the JVM determinstic we will now that the object allocated at 1 was allocated at timestamp 1, so when timestamp 8 comes along we can free object that was allocated at timestamp 1. 

Assume that these are correct and your only job is to use the given resrouces. 