/* Print every ALSA mixer control and its current value.
 *
 * Built for the NW-A30 so the stock firmware's mixer state can be read while
 * it is the thing playing. Rockbox dumps the same list at startup, but that
 * only ever shows the driver's power-on values - it says nothing about what
 * the stock firmware sets, which is what a port that is quieter than the stock
 * firmware needs to know.
 *
 * Run it from the bootloader's "Run script" menu, which starts a file from the
 * user partition, so no reflash is needed. See snapshot_of_mixer.sh.
 *
 *   arm-rockbox-linux-gnueabihf-gcc -O2 -o alsadump alsadump.c -lasound
 */
#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>

static const char *type_name(snd_ctl_elem_type_t type)
{
    switch(type)
    {
        case SND_CTL_ELEM_TYPE_BOOLEAN:    return "BOOL";
        case SND_CTL_ELEM_TYPE_INTEGER:    return "INT";
        case SND_CTL_ELEM_TYPE_INTEGER64:  return "INT64";
        case SND_CTL_ELEM_TYPE_ENUMERATED: return "ENUM";
        case SND_CTL_ELEM_TYPE_BYTES:      return "BYTES";
        default:                           return "?";
    }
}

int main(int argc, char **argv)
{
    const char *card = argc > 1 ? argv[1] : "hw:0";
    snd_ctl_t *ctl;
    int err = snd_ctl_open(&ctl, card, 0);
    if(err < 0)
    {
        fprintf(stderr, "cannot open %s: %s\n", card, snd_strerror(err));
        return 1;
    }

    snd_ctl_elem_list_t *list;
    snd_ctl_elem_list_malloc(&list);
    snd_ctl_elem_list(ctl, list);
    unsigned count = snd_ctl_elem_list_get_count(list);
    snd_ctl_elem_list_set_offset(list, 0);
    snd_ctl_elem_list_alloc_space(list, count);
    snd_ctl_elem_list(ctl, list);

    printf("card %s, %u controls\n", card, count);

    for(unsigned i = 0; i < count; i++)
    {
        snd_ctl_elem_id_t *id;
        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_list_get_id(list, i, id);

        snd_ctl_elem_info_t *info;
        snd_ctl_elem_info_alloca(&info);
        snd_ctl_elem_info_set_id(info, id);
        if(snd_ctl_elem_info(ctl, info) < 0)
            continue;

        snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
        unsigned values = snd_ctl_elem_info_get_count(info);
        const char *name = snd_ctl_elem_info_get_name(info);

        printf("- '%s' %s count=%u", name, type_name(type), values);
        if(type == SND_CTL_ELEM_TYPE_INTEGER)
            printf(" range=%ld..%ld", snd_ctl_elem_info_get_min(info),
                snd_ctl_elem_info_get_max(info));
        printf("\n");

        snd_ctl_elem_value_t *value;
        snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_value_set_id(value, id);
        if(snd_ctl_elem_read(ctl, value) < 0)
        {
            printf("    (unreadable)\n");
            continue;
        }

        printf("   ");
        for(unsigned j = 0; j < values; j++)
        {
            switch(type)
            {
                case SND_CTL_ELEM_TYPE_BOOLEAN:
                    printf(" %d", snd_ctl_elem_value_get_boolean(value, j));
                    break;
                case SND_CTL_ELEM_TYPE_INTEGER:
                    printf(" %ld", snd_ctl_elem_value_get_integer(value, j));
                    break;
                case SND_CTL_ELEM_TYPE_INTEGER64:
                    printf(" %lld",
                        (long long)snd_ctl_elem_value_get_integer64(value, j));
                    break;
                case SND_CTL_ELEM_TYPE_ENUMERATED:
                {
                    unsigned idx = snd_ctl_elem_value_get_enumerated(value, j);
                    snd_ctl_elem_info_set_item(info, idx);
                    if(snd_ctl_elem_info(ctl, info) < 0)
                        printf(" %u", idx);
                    else
                        printf(" '%s'", snd_ctl_elem_info_get_item_name(info));
                    break;
                }
                default:
                    printf(" ?");
                    break;
            }
        }
        printf("\n");
    }

    snd_ctl_elem_list_free_space(list);
    snd_ctl_elem_list_free(list);
    snd_ctl_close(ctl);
    return 0;
}
