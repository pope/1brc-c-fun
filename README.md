# 1brc c fun

Just my attempt at trying to remember a few things in C while using the 1brc as
the motivation for the work here.

## Hash Testing

The hash testing is there to just get a quick idea on how different hashing
algorithms and settings distribute. Mostly there to see how many items have the
same hash code as others items.

To get results, run the following:

```sh
make && ./hash_research \
    | grep "smpl" | awk -F"\t" '{print $3}' \
    | sort -n | uniq -c | sort -n | awk '{print $1}' | uniq -c
```


