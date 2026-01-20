n the paper “Quantifying the Performance of Garbage Collection vs. Explicit Memory Management” by Hertz and Berger (2005), the methodology in Section 2 describes an oracular deallocation system.

To answer your specific questions:

1. Are they doing a "shadow allocation"?
No, they are not doing "shadow allocation" in the traditional sense (where you might allocate a second, parallel structure to mirror the heap for metadata). Instead, they use a trace-based oracle approach.

The Merlin Algorithm: They first run the program to generate a "death trace" using the Merlin algorithm. This trace records exactly when every object becomes unreachable.

The Measurement Run: During the actual performance measurement, they re-execute the program. Because Jikes RVM and the simulator (SimpleScalar) are deterministic, they can identify objects by their allocation order (e.g., the 1,000th object allocated in the trace is exactly the 1,000th object allocated in the measurement run).

The Oracle: Instead of a shadow heap, the "Oracle" is a look-up mechanism. When the program reaches a point where the trace says "Object X is now dead," the system immediately calls free(X).

The simulator allows them to "charge" the CPU cycles for the malloc and free calls while not charging for the overhead of consulting the oracle itself. This ensures the results reflect the cost of the manual memory management, not the cost of their experimental tool.

2. Is the memory "On-Heap" or "Off-Heap"?
In the context of this study, the distinction between on-heap and off-heap becomes slightly blurred because they are replacing the JVM's standard memory manager entirely for the EMM tests.

The Jikes RVM Context: Jikes RVM is a "Java-in-Java" VM where the "heap" is typically a large block of memory managed by the MMTk (Memory Management Toolkit).

Explicit Management Run: When they call malloc and free, they are using conventional C-style allocators (like Lea’s allocator) to manage the memory where the Java objects reside.

The "Heap" Location: The RVM is absolutely "using that memory"—it is where the object's fields, headers, and data are stored. However, from the perspective of the JVM's original design, this memory is essentially "off-heap" because it is not managed by a Garbage Collector. It exists in the address space managed by the manual malloc library.

Summary Table
Feature	Hertz & Berger Methodology (Section 2)
Identification	Uses Allocation Order to match objects to the death trace.
Shadowing?	No shadow heap; metadata is stored in an external trace file.
Memory Source	malloc manages the actual memory used by Java objects.
Simulation	Uses SimpleScalar to subtract oracle lookup costs, leaving only the malloc/free costs.

In short: The Jikes RVM is using that memory to store its objects, but the policy and mechanism for reclaiming that memory have been moved from the internal GC to an external malloc/free library that treats the Java objects exactly like C structs.

In their methodology, Hertz and Berger go to great lengths to ensure that the "Explicit Memory Management" (EMM) version of the Java program is as realistic as possible, specifically regarding how memory is actually utilized.Object Headers and LayoutThe paper addresses a critical difference between Jikes RVM's garbage-collected heap and standard C malloc: the header overhead.Standard GC Header: In Jikes RVM, every object typically has a multi-word header used for locking, hashing, and GC metadata (like mark bits).The EMM Header: When they switch to explicit management, they maintain these Java headers so the VM can still function (e.g., for virtual method calls and synchronization). However, when malloc is called, the allocator adds its own internal metadata (usually a size word before the payload).The BIBOP Optimization: To prevent the EMM version from being unfairly penalized by "double headers" (JVM header + malloc header), they sometimes use BiBoP (Big Bag of Pages) style allocators. This moves the allocation metadata to the page level rather than storing a size-word directly in front of every single object.Is the memory used by Jikes RVM or "Off-Heap"?It is definitively on-heap in terms of data usage, but off-heap in terms of management.Placement: The memory returned by malloc is the actual memory where the Java object’s fields are stored. If a Java program executes myObject.field = 5, the CPU writes that 5 into the memory block provided by malloc.Jikes RVM Awareness: The Jikes RVM "knows" these addresses. During the measurement run, the JVM’s allocation site is redirected. Instead of asking the MMTk (Memory Management Toolkit) for a block of memory from a managed nursery, it calls out to the malloc library.The "Off-Heap" Illusion: Because this memory is not inside the traditional "GC Heap" range that the collector would normally scan, it acts like off-heap memory. However, since the paper uses an Oracle, there is no need for the JVM to "track" this memory for safety—the Oracle tells the system exactly when to free it based on the previous Merlin trace.Summary of the "Malloc/Free" mechanismThe authors essentially "hijacked" the new opcode in the JVM.FeatureGC RunExplicit (EMM) RunAllocatorMMTk (Bump-pointer or Mark-Sweep)malloc (Lea's allocator or Hoard)MetadataGC-specific bits in object headerExternal trace/Oracle + Allocator metadataDeallocationOccurs during GC cycles (Tracing)Occurs instantly via free based on traceVisibilityJVM manages the entire heap blockJVM uses individual blocks from mallocBy doing this, they can prove that if a Java developer could call free, the performance would follow the trends they discovered (the 3x–5x memory rule)