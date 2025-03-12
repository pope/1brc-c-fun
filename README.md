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

And here's for the largest word
```sh
cat weather-stations.txt | awk '{print length($0)}' | sort -n
```

What I discovered with Hash Testing is that with a big enough bucket, I will
get collisions - so I was unable to make a perfect hash here. BUT, there are
not any collisions in the hashes themselves - even in a 32bit space.

## Data Assumptions

  - We will parse the temp as an integer by multiplying the value by 10
  - When we parse out a temp, it can fit within 11 bits (2048) for the range of
    999 to 999. This means we can use a `short` to hold these.
  - The max length of a weather station name can fit in 9 bytes (512 bits, 64
    characters). This should mean and an `unsigned short` is enough to hold the
    length of the key.
  - The maximum number of weather stations fits within 14 bits (16384) of
    information. That should mean that as an array index, we only need an
    `unsigned short` to access all possible stations.
    - This does present a fair number of collisions however for a hashtable.
    - 16 bits of space results in 30% collisions.
    - 17 bits of space results in 15% collisions. Though will need more space
      to hold the index.
    - 18 bits of space results in 8% collisions. Ditto.
    - These are worse case numbers though as the 1br data doesn't have all of
      the weather stations in it.
  - Using the simple hash function, we can create unique hash codes that all
    fit within 32 bits - `unsigned int` - where all values are unique for the 
    known stations.
  - The count of all the items is less than 30 bits, and thus can fit within an
    `unsigned int`

## Links

  - https://curc.readthedocs.io/en/latest/programming/OpenMP-C.html
  - https://people.cs.pitt.edu/~melhem/courses/xx45p/OpenMp.pdf
  - https://tildesites.bowdoin.edu/~ltoma/teaching/cs3225-GIS/fall17/Lectures/openmp.html
