target datalayout = "e-p:32:32-p1:64:64-p2:64:64-p3:32:32-p4:64:64-p5:32:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64"
target triple = "amdgcn--amdhsa"


define void @__threadfence() #1 {
    fence syncscope(2) seq_cst
    ret void
}

define void @__threadfence_block()  #1 {
    fence syncscope(3) seq_cst
    ret void
}

attributes #1 = { alwaysinline nounwind }
